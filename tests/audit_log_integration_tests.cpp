#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <thread>
#include <vector>

#include "persist/audit_log_writer.hpp"

namespace {

std::vector<std::byte> read_file_bytes(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::vector<char> tmp((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::byte> data;
    data.reserve(tmp.size());
    for (char c : tmp) {
        data.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return data;
}

std::vector<persist::DecodedRecord> parse_dir(const std::filesystem::path& dir,
                                              persist::AuditLogCounters& counters) {
    std::vector<persist::DecodedRecord> out;
    for (auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        auto data = read_file_bytes(entry.path());
        std::size_t offset = 0;
        while (offset < data.size()) {
            persist::DecodedRecord rec{};
            auto err = persist::decode_record(std::span<const std::byte>(data.data() + offset, data.size() - offset),
                                              rec, &counters);
            if (err == persist::DecodeError::Ok) {
                out.push_back(rec);
                offset += persist::record_size_from_payload(rec.payload_len);
            } else if (err == persist::DecodeError::TruncatedAtEnd) {
                break; // graceful EOF
            } else {
                ADD_FAILURE() << "Decode error " << static_cast<int>(err) << " at file " << entry.path();
                break;
            }
        }
    }
    return out;
}

persist::AuditLogConfig default_test_cfg(const std::filesystem::path& dir) {
    persist::AuditLogConfig cfg;
    cfg.output_dir = dir;
    cfg.rotate_max_bytes = 16 * 1024 * 1024;
    cfg.rotate_interval = std::chrono::hours(1);
    cfg.batch_max_bytes = 1024 * 1024;
    cfg.batch_max_records = 64;
    cfg.flush_idle_timeout = std::chrono::milliseconds(2);
    cfg.staging_buffer_bytes = 64 * 1024;
    return cfg;
}

core::Divergence make_div(std::uint64_t key) {
    core::Divergence d{};
    d.key = key;
    d.type = core::DivergenceType::StateMismatch;
    d.internal_status = core::OrdStatus::Working;
    d.dropcopy_status = core::OrdStatus::Filled;
    d.internal_cum_qty = 1;
    d.dropcopy_cum_qty = 2;
    d.internal_avg_px = 3;
    d.dropcopy_avg_px = 4;
    d.internal_ts = key * 10;
    d.dropcopy_ts = key * 20;
    return d;
}

core::SequenceGapEvent make_gap(std::uint64_t seq) {
    core::SequenceGapEvent g{};
    g.source = core::Source::Primary;
    g.session_id = 7;
    g.expected_seq = seq;
    g.seen_seq = seq + 2;
    g.kind = core::GapKind::Gap;
    g.detect_ts = seq * 100;
    return g;
}

TEST(AuditLogIntegration, RoundTripEncodeDecode) {
    auto tmpdir = std::filesystem::temp_directory_path() / "audit_integration";
    std::filesystem::remove_all(tmpdir);
    std::filesystem::create_directories(tmpdir);

    persist::DivergenceAuditRing div_ring;
    persist::GapAuditRing gap_ring;
    persist::AuditLogCounters ctrs;
    persist::AuditLogWriter writer(div_ring, gap_ring, ctrs, default_test_cfg(tmpdir));
    writer.start();

    constexpr int kDiv = 8;
    constexpr int kGap = 6;
    for (int i = 0; i < kDiv; ++i) {
        ASSERT_TRUE(div_ring.try_push(make_div(static_cast<std::uint64_t>(i + 1))));
    }
    for (int i = 0; i < kGap; ++i) {
        ASSERT_TRUE(gap_ring.try_push(make_gap(static_cast<std::uint64_t>(i + 1))));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    writer.stop();

    persist::AuditLogCounters parse_ctrs;
    auto parsed = parse_dir(tmpdir, parse_ctrs);
    EXPECT_EQ(parsed.size(), static_cast<std::size_t>(kDiv + kGap));
    EXPECT_EQ(ctrs.writer_drop_divergence.load(), 0);
    EXPECT_EQ(ctrs.writer_drop_gaps.load(), 0);
    EXPECT_EQ(ctrs.audit_drop_divergence.load(), 0);
    EXPECT_EQ(ctrs.audit_drop_gaps.load(), 0);
    EXPECT_EQ(parse_ctrs.audit_parse_errors_crc.load(), 0);
    EXPECT_EQ(parse_ctrs.audit_parse_errors_truncated.load(), 0);
}

TEST(AuditLogIntegration, BackpressureDropsCounted) {
    auto tmpdir = std::filesystem::temp_directory_path() / "audit_backpressure";
    std::filesystem::remove_all(tmpdir);
    std::filesystem::create_directories(tmpdir);

    persist::DivergenceAuditRing div_ring;
    persist::GapAuditRing gap_ring;
    persist::AuditLogCounters ctrs;
    // Flood ring before writer starts to force try_push drops.
    std::size_t drops = 0;
    core::Divergence div = make_div(100);
    for (std::size_t i = 0; i < 6000; ++i) {
        if (!div_ring.try_push(div)) {
            ++drops;
            ctrs.audit_drop_divergence.fetch_add(1, std::memory_order_relaxed);
        }
    }

    persist::AuditLogWriter writer(div_ring, gap_ring, ctrs, default_test_cfg(tmpdir));
    writer.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    writer.stop();

    persist::AuditLogCounters parse_ctrs;
    auto parsed = parse_dir(tmpdir, parse_ctrs);
    const std::size_t expected_records = 6000 - drops;
    EXPECT_EQ(parsed.size(), expected_records);
    EXPECT_EQ(ctrs.audit_drop_divergence.load(), drops);
}

} // namespace
