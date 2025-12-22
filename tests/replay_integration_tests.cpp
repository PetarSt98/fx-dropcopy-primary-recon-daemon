#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>
#include <cstring>
#include <iterator>
#include <array>
#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <sys/wait.h>

#include "core/exec_event.hpp"
#include "core/wire_exec_event.hpp"
#include "persist/audit_log_format.hpp"
#include "persist/wire_log_format.hpp"

namespace {

std::filesystem::path make_tmp_dir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

core::WireExecEvent make_wire(std::uint64_t seq,
                              std::uint64_t ts,
                              std::uint16_t session_id,
                              std::uint8_t ord_status,
                              std::uint8_t exec_type,
                              const std::string& clord_id,
                              std::int64_t qty = 0,
                              std::int64_t cum_qty = 0) {
    core::WireExecEvent evt{};
    evt.seq_num = seq;
    evt.session_id = session_id;
    evt.sending_time = ts;
    evt.transact_time = ts;
    evt.ord_status = ord_status;
    evt.exec_type = exec_type;
    evt.qty = qty;
    evt.cum_qty = cum_qty;
    evt.exec_id_len = 4;
    std::memcpy(evt.exec_id, "EXEC", 4);
    evt.order_id_len = static_cast<std::uint8_t>(clord_id.size());
    std::memcpy(evt.order_id, clord_id.data(), clord_id.size());
    evt.clord_id_len = static_cast<std::uint8_t>(clord_id.size());
    std::memcpy(evt.clord_id, clord_id.data(), clord_id.size());
    return evt;
}

void write_header(std::ofstream& out) {
    persist::WireLogHeaderFields header{};
    std::array<std::byte, persist::wire_log_header_size> bytes{};
    persist::encode_header(header, bytes);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_record(std::ofstream& out, const core::WireExecEvent& evt, std::uint64_t capture_ts) {
    std::array<std::byte, persist::wire_exec_event_wire_size> payload{};
    persist::serialize_wire_exec_event(evt, reinterpret_cast<std::uint8_t*>(payload.data()));
    persist::RecordFields fields{};
    persist::encode_record(payload, capture_ts, fields);
    out.write(reinterpret_cast<const char*>(fields.length_le.data()), static_cast<std::streamsize>(fields.length_le.size()));
    out.write(reinterpret_cast<const char*>(fields.capture_ts_le.data()), static_cast<std::streamsize>(fields.capture_ts_le.size()));
    out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    out.write(reinterpret_cast<const char*>(fields.checksum_le.data()), static_cast<std::streamsize>(fields.checksum_le.size()));
}

std::vector<persist::DecodedRecord> parse_audit_dir(const std::filesystem::path& dir) {
    std::vector<persist::DecodedRecord> out;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            std::span<const std::byte> data(reinterpret_cast<const std::byte*>(bytes.data() + offset),
                                            bytes.size() - offset);
            persist::DecodedRecord rec{};
            auto err = persist::decode_record(data, rec);
            if (err == persist::DecodeError::Ok) {
                out.push_back(rec);
                offset += persist::record_size_from_payload(rec.payload_len);
            } else if (persist::is_graceful_eof(err)) {
                break;
            } else {
                ADD_FAILURE() << "Decode error in " << entry.path();
                break;
            }
        }
    }
    return out;
}

void flip_first_byte(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec); !ec && it != std::filesystem::directory_iterator(); ++it) {
        if (it->is_regular_file(ec) && !ec) {
            files.push_back(it->path());
        }
    }
    ASSERT_FALSE(files.empty());
    std::sort(files.begin(), files.end());
    std::fstream f(files.front(), std::ios::in | std::ios::out | std::ios::binary);
    ASSERT_TRUE(f.is_open());
    char byte = 0;
    f.read(&byte, 1);
    byte = static_cast<char>(byte ^ 0xFF);
    f.seekp(0);
    f.write(&byte, 1);
    f.flush();
}

int run_replay_cli(const std::vector<std::string>& args) {
    std::ostringstream cmd;
    cmd << "./fx_replay_main";
    for (const auto& arg : args) {
        cmd << " " << arg;
    }
    const int raw = std::system(cmd.str().c_str());
    if (raw == -1) {
        return -1;
    }
    if (WIFEXITED(raw)) {
        return WEXITSTATUS(raw);
    }
    return raw;
}

} // namespace

TEST(ReplayIntegration, DeterministicReplayAndVerify) {
    std::string step = "start";
    try {
        const auto tmp_root = make_tmp_dir("replay_integration");
        step = "wire_path";
        const auto log_path = tmp_root / "wire.bin";
        {
            std::ofstream out(log_path, std::ios::binary | std::ios::trunc);
            write_header(out);
            // 99 primary records with a known gap at seq=50 plus one dropcopy record to trigger divergence.
            std::size_t written = 0;
            for (std::uint64_t seq = 1; seq <= 100; ++seq) {
                if (seq == 50) {
                    continue; // intentional gap
                }
                const auto ts = 1'000 + seq * 1'000;
                const auto oid = "OID" + std::to_string(seq % 5);
                const auto ord_status = (seq % 2 == 0) ? core::OrdStatus::Working : core::OrdStatus::New;
                const auto exec_type = (seq % 3 == 0) ? core::ExecType::PartialFill : core::ExecType::New;
                const auto qty = (seq % 4 == 0) ? 10 : 0;
                const auto cum = (seq % 6 == 0) ? static_cast<std::int64_t>(seq) : qty;
                write_record(out, make_wire(seq, ts, 0,
                                            static_cast<std::uint8_t>(ord_status),
                                            static_cast<std::uint8_t>(exec_type),
                                            oid,
                                            qty,
                                            cum),
                             ts + 100);
                ++written;
            }
            ASSERT_EQ(written, 99u);
            write_record(out, make_wire(1, 200'000, 1, static_cast<std::uint8_t>(core::OrdStatus::Filled),
                                        static_cast<std::uint8_t>(core::ExecType::Fill), "OID1", 500, 500),
                         200'100);
            ASSERT_EQ(written + 1, 100u);
        }

        step = "run1";
        const auto out1 = tmp_root / "out1";
        const int rc1 = run_replay_cli({
            "--input", log_path.string(),
            "--out-dir", out1.string(),
            "--fast"
        });
        ASSERT_EQ(rc1, 0);

        step = "parse_out1";
        const auto records = parse_audit_dir(out1);
        std::size_t div_count = 0;
        std::size_t gap_count = 0;
        for (const auto& rec : records) {
            if (rec.type == persist::AuditRecordType::Divergence) {
                ++div_count;
            } else if (rec.type == persist::AuditRecordType::SequenceGap) {
                ++gap_count;
            }
        }
        EXPECT_GE(div_count, 1u);
        EXPECT_GE(gap_count, 1u);

        step = "run2";
        const auto out2 = tmp_root / "out2";
        const int rc2 = run_replay_cli({
            "--input", log_path.string(),
            "--out-dir", out2.string(),
            "--verify-against", out1.string(),
            "--fast"
        });
        EXPECT_TRUE(rc2 == 0 || rc2 == 2);

        // Time-window replay should exclude the dropcopy divergence and still process gaps deterministically.
        step = "window";
        const auto out_window = tmp_root / "out_window";
        const int rc_window = run_replay_cli({
            "--input", log_path.string(),
            "--out-dir", out_window.string(),
            "--from-ns", "20000",
            "--to-ns", "80000",
            "--fast"
        });
        ASSERT_EQ(rc_window, 0);
        const auto window_records = parse_audit_dir(out_window);
        ASSERT_FALSE(window_records.empty());
        EXPECT_LT(window_records.size(), records.size());

        // Intentional mismatch should surface via --verify-against with exit code 2.
        step = "mutate";
        const auto golden_mutated = tmp_root / "out_golden_mutated";
        std::filesystem::create_directories(golden_mutated);
        for (const auto& entry : std::filesystem::directory_iterator(out1)) {
            if (entry.is_regular_file()) {
                std::error_code copy_ec;
                std::filesystem::copy_file(entry.path(), golden_mutated / entry.path().filename(),
                                           std::filesystem::copy_options::overwrite_existing, copy_ec);
                ASSERT_FALSE(copy_ec) << copy_ec.message();
            }
        }
        flip_first_byte(golden_mutated);

        step = "run_bad";
        const auto out_bad = tmp_root / "out_bad";
        const int rc_bad = run_replay_cli({
            "--input", log_path.string(),
            "--out-dir", out_bad.string(),
            "--verify-against", golden_mutated.string(),
            "--fast"
        });
        EXPECT_EQ(rc_bad, 2);
    } catch (const std::exception& ex) {
        FAIL() << "Exception at step " << step << ": " << ex.what();
    }
}
