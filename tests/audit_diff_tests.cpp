#include "persist/audit_diff.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <span>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#include "persist/audit_log_format.hpp"

using namespace persist;

namespace {

std::filesystem::path make_temp_dir(const std::string& name) {
    auto base = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(base, ec);
    std::filesystem::create_directories(base);
    return base;
}

std::vector<std::byte> make_divergence_record(core::OrderKey key, core::DivergenceType type) {
    core::Divergence div{};
    div.key = key;
    div.type = type;
    div.internal_status = core::OrdStatus::Working;
    div.dropcopy_status = core::OrdStatus::Working;
    div.internal_cum_qty = 1;
    div.dropcopy_cum_qty = 2;
    div.internal_avg_px = 3;
    div.dropcopy_avg_px = 4;
    div.internal_ts = 10;
    div.dropcopy_ts = 11;

    std::vector<std::byte> buf(record_size_from_payload(divergence_payload_v1_size));
    std::size_t written = 0;
    EXPECT_TRUE(encode_divergence_record_v1(div, buf, written));
    buf.resize(written);
    return buf;
}

void write_records(const std::filesystem::path& path,
                   const std::vector<std::vector<std::byte>>& records) {
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(out.is_open());
    for (const auto& rec : records) {
        out.write(reinterpret_cast<const char*>(rec.data()),
                  static_cast<std::streamsize>(rec.size()));
    }
}

std::size_t current_rss_bytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
        return static_cast<std::size_t>(counters.PeakWorkingSetSize);
    }
    return 0;
#else
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        return static_cast<std::size_t>(usage.ru_maxrss) * 1024; // ru_maxrss in KB
    }
    return 0;
#endif
}

} // namespace

static void write_whitelist_file(const std::filesystem::path& path, const std::string& body) {
    std::ofstream out(path);
    ASSERT_TRUE(out.is_open());
    out << body;
}

TEST(AuditDiffTests, ExactMatchDirectories) {
    auto exp = make_temp_dir("audit_diff_exp1");
    auto act = make_temp_dir("audit_diff_act1");
    auto record = make_divergence_record(1, core::DivergenceType::StateMismatch);
    write_records(exp / "audit.bin", {record});
    write_records(act / "audit.bin", {record});

    DiffStats stats;
    DiffOptions options;
    std::string report;
    const auto result = diff_directories(exp, act, options, stats, report);
    EXPECT_EQ(result, DiffResult::Match);
    EXPECT_EQ(stats.mismatches, 0u);
    EXPECT_EQ(stats.files_compared, 1u);
}

TEST(AuditDiffTests, SingleByteDiff) {
    auto exp = make_temp_dir("audit_diff_exp2");
    auto act = make_temp_dir("audit_diff_act2");
    auto record = make_divergence_record(2, core::DivergenceType::StateMismatch);
    auto record_diff = record;
    record_diff[0] = std::byte{static_cast<unsigned char>(static_cast<uint8_t>(record_diff[0]) ^ 0xFF)};
    write_records(exp / "audit.bin", {record});
    write_records(act / "audit.bin", {record_diff});

    DiffStats stats;
    DiffOptions options;
    std::string report;
    const auto result = diff_directories(exp, act, options, stats, report);
    EXPECT_EQ(result, DiffResult::Mismatch);
    EXPECT_GT(stats.mismatches, 0u);
    EXPECT_NE(report.find("Offset"), std::string::npos);
}

TEST(AuditDiffTests, WhitelistedDiff) {
    auto exp = make_temp_dir("audit_diff_exp3");
    auto act = make_temp_dir("audit_diff_act3");
    auto record_exp = make_divergence_record(3, core::DivergenceType::StateMismatch);
    auto record_act = make_divergence_record(3, core::DivergenceType::QuantityMismatch);
    write_records(exp / "audit.bin", {record_exp});
    write_records(act / "audit.bin", {record_act});

    auto wl_dir = make_temp_dir("audit_diff_wl3");
    auto whitelist_path = wl_dir / "whitelist.json";
    write_whitelist_file(whitelist_path, R"({
        "version": 1,
        "rules": [
            {"type":"ignore_divergence_type","divergence_type":"QuantityMismatch","reason":"test"}
        ]
    })");

    DiffStats stats;
    DiffOptions options;
    options.byte_for_byte = false;
    options.whitelist_path = whitelist_path;
    std::string report;
    const auto result = diff_directories(exp, act, options, stats, report);
    EXPECT_EQ(result, DiffResult::Match) << report;
    EXPECT_EQ(stats.mismatches, 0u) << report;
    EXPECT_GT(stats.whitelisted, 0u);
}

TEST(AuditDiffTests, MissingExpectedFile) {
    auto exp = make_temp_dir("audit_diff_exp4");
    auto act = make_temp_dir("audit_diff_act4");
    auto record = make_divergence_record(4, core::DivergenceType::StateMismatch);
    write_records(act / "audit.bin", {record});

    DiffStats stats;
    DiffOptions options;
    std::string report;
    const auto result = diff_directories(exp, act, options, stats, report);
    EXPECT_EQ(result, DiffResult::Mismatch);
    EXPECT_GT(stats.mismatches, 0u);
}

TEST(AuditDiffTests, ExtraActualFileAllowed) {
    auto exp = make_temp_dir("audit_diff_exp5");
    auto act = make_temp_dir("audit_diff_act5");
    auto record = make_divergence_record(5, core::DivergenceType::StateMismatch);
    write_records(exp / "audit.bin", {record});
    write_records(act / "audit.bin", {record});
    write_records(act / "debug_tmp.bin", {record});

    auto wl_dir = make_temp_dir("audit_diff_wl5");
    auto whitelist_path = wl_dir / "whitelist.json";
    write_whitelist_file(whitelist_path, R"({
        "version": 1,
        "rules": [
            {"type":"allow_extra_files","patterns":["debug_*"],"reason":"allow debug"}
        ]
    })");

    DiffStats stats;
    DiffOptions options;
    options.whitelist_path = whitelist_path;
    std::string report;
    const auto result = diff_directories(exp, act, options, stats, report);
    EXPECT_EQ(result, DiffResult::Match) << report;
    EXPECT_EQ(stats.mismatches, 0u) << report;
    EXPECT_GT(stats.whitelisted, 0u);
}

TEST(AuditDiffTests, StreamingLargeFileMemoryBounded) {
    auto exp = make_temp_dir("audit_diff_exp6");
    auto act = make_temp_dir("audit_diff_act6");

    const std::size_t record_size = record_size_from_payload(divergence_payload_v1_size);
    const std::size_t target_bytes = 110ull * 1024ull * 1024ull;
    const std::size_t num_records = target_bytes / record_size;

    std::vector<std::byte> rec = make_divergence_record(6, core::DivergenceType::StateMismatch);
    {
        std::ofstream out_exp(exp / "audit.bin", std::ios::binary);
        std::ofstream out_act(act / "audit.bin", std::ios::binary);
        ASSERT_TRUE(out_exp.is_open());
        ASSERT_TRUE(out_act.is_open());
        for (std::size_t i = 0; i < num_records; ++i) {
            out_exp.write(reinterpret_cast<const char*>(rec.data()), static_cast<std::streamsize>(rec.size()));
            out_act.write(reinterpret_cast<const char*>(rec.data()), static_cast<std::streamsize>(rec.size()));
        }
    }

    DiffStats stats;
    DiffOptions options;
    options.byte_for_byte = false; // Force record streaming path.
    std::string report;
    const auto result = diff_directories(exp, act, options, stats, report);
    const auto after = current_rss_bytes();
    ASSERT_EQ(result, DiffResult::Match);
    EXPECT_EQ(stats.mismatches, 0u);
    EXPECT_LT(after, 300ull * 1024ull * 1024ull);
    EXPECT_GT(stats.records_compared, 0u);
}
