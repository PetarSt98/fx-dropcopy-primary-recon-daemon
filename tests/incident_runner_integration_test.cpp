#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
#include <vector>
#include <unistd.h>

#include <gtest/gtest.h>

#include "persist/wire_log_format.hpp"

namespace incident_runner {
int run_incident_runner_cli(const std::vector<std::string>& args);
}

namespace {

std::filesystem::path make_temp_dir(const std::string& prefix) {
    std::string templ = "/tmp/" + prefix + "XXXXXX";
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    char* dir = mkdtemp(buf.data());
    if (!dir) {
        throw std::runtime_error("mkdtemp failed");
    }
    return std::filesystem::path(dir);
}

bool write_wire_log_with_gap(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    persist::WireLogHeaderFields header{};
    std::array<std::byte, persist::wire_log_header_size> header_bytes{};
    persist::encode_header(header, header_bytes);
    out.write(reinterpret_cast<const char*>(header_bytes.data()), static_cast<std::streamsize>(header_bytes.size()));

    auto write_record = [&](std::uint64_t seq, std::uint64_t capture_ts) {
        core::WireExecEvent evt{};
        evt.seq_num = seq;
        evt.session_id = 2; // even -> primary
        evt.price_micro = 1000;
        evt.qty = 10;
        evt.cum_qty = 10;
        evt.sending_time = capture_ts;
        evt.transact_time = capture_ts;
        const char exec_id[] = "EXEC";
        const char order_id[] = "ORDER";
        const char clord_id[] = "CLORD";
        std::memcpy(evt.exec_id, exec_id, sizeof(exec_id) - 1);
        evt.exec_id_len = sizeof(exec_id) - 1;
        std::memcpy(evt.order_id, order_id, sizeof(order_id) - 1);
        evt.order_id_len = sizeof(order_id) - 1;
        std::memcpy(evt.clord_id, clord_id, sizeof(clord_id) - 1);
        evt.clord_id_len = sizeof(clord_id) - 1;

        std::array<std::byte, persist::wire_exec_event_wire_size> payload{};
        persist::serialize_wire_exec_event(evt, reinterpret_cast<std::uint8_t*>(payload.data()));
        persist::RecordFields fields{};
        persist::encode_record(payload, capture_ts, fields);
        out.write(reinterpret_cast<const char*>(fields.length_le.data()), sizeof(fields.length_le));
        out.write(reinterpret_cast<const char*>(fields.capture_ts_le.data()), sizeof(fields.capture_ts_le));
        out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        out.write(reinterpret_cast<const char*>(fields.checksum_le.data()), sizeof(fields.checksum_le));
    };

    write_record(1, 100);
    write_record(3, 200);
    return out.good();
}

void write_incident_spec(const std::filesystem::path& path, const std::string& id, const std::filesystem::path& wire) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out << "{\n"
        << "  \"id\": \"" << id << "\",\n"
        << "  \"description\": \"integration test\",\n"
        << "  \"wire_inputs\": [\n"
        << "    {\"path\": \"" << wire.filename().string() << "\", \"from_ns\": 0, \"to_ns\": 500}\n"
        << "  ],\n"
        << "  \"replay\": {\"speed\": \"fast\", \"max_records\": 0}\n"
        << "}\n";
}

void write_config(const std::filesystem::path& path, std::size_t max_records) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out << "{\n"
        << "  \"max_records\": " << max_records << "\n"
        << "}\n";
}

void write_whitelist_allow_extra(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out << "{\n"
        << "  \"version\": 1,\n"
        << "  \"rules\": [\n"
        << "    {\"type\": \"allow_extra_files\", \"patterns\": [\"audit_*\"]},\n"
        << "    {\"type\": \"ignore_divergence_type\", \"divergence_type\": \"StateMismatch\"},\n"
        << "    {\"type\": \"ignore_divergence_type\", \"divergence_type\": \"QuantityMismatch\"},\n"
        << "    {\"type\": \"ignore_divergence_type\", \"divergence_type\": \"TimingAnomaly\"},\n"
        << "    {\"type\": \"ignore_divergence_type\", \"divergence_type\": \"MissingFill\"},\n"
        << "    {\"type\": \"ignore_divergence_type\", \"divergence_type\": \"PhantomOrder\"}\n"
        << "  ]\n"
        << "}\n";
}

int run_runner(const std::vector<std::string>& args) {
    return incident_runner::run_incident_runner_cli(args);
}

} // namespace

TEST(IncidentRunnerIntegration, FreshGoldenAndMatch) {
    const auto root = make_temp_dir("incident_runner_");
    const auto incident_dir = root / "INC-TEST-1";
    std::filesystem::create_directories(incident_dir);
    const auto wire = incident_dir / "wire.bin";
    ASSERT_TRUE(write_wire_log_with_gap(wire));

    write_incident_spec(incident_dir / "incident.json", "INC-TEST-1", wire);
    write_config(incident_dir / "baseline_config.json", 0);
    write_config(incident_dir / "candidate_config.json", 0);

    const auto work_dir = root / "work";

    int code = run_runner({
        "--spec", (incident_dir / "incident.json").string(),
        "--refresh-golden",
        "--work-dir", work_dir.string(),
        "--baseline-config", (incident_dir / "baseline_config.json").string(),
        "--candidate-config", (incident_dir / "candidate_config.json").string()
    });
    EXPECT_EQ(code, 0);

    code = run_runner({
        "--spec", (incident_dir / "incident.json").string(),
        "--work-dir", work_dir.string(),
        "--baseline-config", (incident_dir / "baseline_config.json").string(),
        "--candidate-config", (incident_dir / "candidate_config.json").string()
    });
    EXPECT_EQ(code, 0);
}

TEST(IncidentRunnerIntegration, MismatchWithoutWhitelist) {
    const auto root = make_temp_dir("incident_runner_mismatch_");
    const auto incident_dir = root / "INC-TEST-2";
    std::filesystem::create_directories(incident_dir);
    const auto wire = incident_dir / "wire.bin";
    ASSERT_TRUE(write_wire_log_with_gap(wire));

    write_incident_spec(incident_dir / "incident.json", "INC-TEST-2", wire);
    write_config(incident_dir / "baseline_config.json", 0);
    write_config(incident_dir / "candidate_config.json", 1); // limit candidate to suppress gap

    const auto work_dir = root / "work";
    int code = run_runner({
        "--spec", (incident_dir / "incident.json").string(),
        "--refresh-golden",
        "--work-dir", work_dir.string(),
        "--baseline-config", (incident_dir / "baseline_config.json").string(),
        "--candidate-config", (incident_dir / "candidate_config.json").string()
    });
    ASSERT_EQ(code, 0);

    code = run_runner({
        "--spec", (incident_dir / "incident.json").string(),
        "--work-dir", work_dir.string(),
        "--baseline-config", (incident_dir / "baseline_config.json").string(),
        "--candidate-config", (incident_dir / "candidate_config.json").string()
    });
    EXPECT_EQ(code, 2);
}

TEST(IncidentRunnerIntegration, MismatchWhitelisted) {
    const auto root = make_temp_dir("incident_runner_whitelist_");
    const auto incident_dir = root / "INC-TEST-3";
    std::filesystem::create_directories(incident_dir);
    const auto wire = incident_dir / "wire.bin";
    ASSERT_TRUE(write_wire_log_with_gap(wire));

    write_incident_spec(incident_dir / "incident.json", "INC-TEST-3", wire);
    write_config(incident_dir / "baseline_config.json", 1);   // truncate, likely no gaps
    write_config(incident_dir / "candidate_config.json", 0);  // full, produces gap file
    write_whitelist_allow_extra(incident_dir / "whitelist.json");

    const auto work_dir = root / "work";
    int code = run_runner({
        "--spec", (incident_dir / "incident.json").string(),
        "--refresh-golden",
        "--work-dir", work_dir.string(),
        "--baseline-config", (incident_dir / "baseline_config.json").string(),
        "--candidate-config", (incident_dir / "candidate_config.json").string(),
        "--whitelist", (incident_dir / "whitelist.json").string()
    });
    ASSERT_EQ(code, 0);

    for (const auto& entry : std::filesystem::directory_iterator(work_dir / "golden")) {
        if (entry.is_regular_file() && entry.path().filename().string().rfind("audit", 0) == 0) {
            std::filesystem::remove(entry.path());
        }
    }

    code = run_runner({
        "--spec", (incident_dir / "incident.json").string(),
        "--work-dir", work_dir.string(),
        "--baseline-config", (incident_dir / "baseline_config.json").string(),
        "--candidate-config", (incident_dir / "candidate_config.json").string(),
        "--whitelist", (incident_dir / "whitelist.json").string()
    });
    EXPECT_EQ(code, 0);
}

TEST(IncidentRunnerIntegration, MissingGoldenIsError) {
    const auto root = make_temp_dir("incident_runner_missing_");
    const auto incident_dir = root / "INC-TEST-4";
    std::filesystem::create_directories(incident_dir);
    const auto wire = incident_dir / "wire.bin";
    ASSERT_TRUE(write_wire_log_with_gap(wire));

    write_incident_spec(incident_dir / "incident.json", "INC-TEST-4", wire);
    write_config(incident_dir / "baseline_config.json", 0);
    write_config(incident_dir / "candidate_config.json", 0);

    const auto work_dir = root / "work";
    // Intentionally do not refresh to create golden
    int code = run_runner({
        "--spec", (incident_dir / "incident.json").string(),
        "--work-dir", work_dir.string(),
        "--baseline-config", (incident_dir / "baseline_config.json").string(),
        "--candidate-config", (incident_dir / "candidate_config.json").string()
    });
    EXPECT_EQ(code, 3);
}

TEST(IncidentRunnerIntegration, InvalidSpecFails) {
    const auto root = make_temp_dir("incident_runner_bad_spec_");
    const auto incident_dir = root / "INC-TEST-5";
    std::filesystem::create_directories(incident_dir);

    std::ofstream bad(incident_dir / "incident.json", std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(bad.is_open());
    bad << "{ invalid json ";
    bad.close();

    write_config(incident_dir / "baseline_config.json", 0);
    write_config(incident_dir / "candidate_config.json", 0);

    const auto work_dir = root / "work";
    int code = run_runner({
        "--spec", (incident_dir / "incident.json").string(),
        "--work-dir", work_dir.string(),
        "--baseline-config", (incident_dir / "baseline_config.json").string(),
        "--candidate-config", (incident_dir / "candidate_config.json").string()
    });
    EXPECT_EQ(code, 3);
}
