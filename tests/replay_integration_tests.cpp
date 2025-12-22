#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>
#include <cstring>
#include <iterator>
#include <array>

#include "api/replay.hpp"
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

} // namespace

TEST(ReplayIntegration, DeterministicReplayAndVerify) {
    const auto tmp_root = make_tmp_dir("replay_integration");
    const auto log_path = tmp_root / "wire.bin";
    {
        std::ofstream out(log_path, std::ios::binary | std::ios::trunc);
        write_header(out);
        // Primary (session_id even) gap between seq 1 and 3 to trigger gap detection.
        write_record(out, make_wire(1, 1'000, 0, static_cast<std::uint8_t>(core::OrdStatus::New),
                                    static_cast<std::uint8_t>(core::ExecType::New), "OID1"),
                     1'100);
        write_record(out, make_wire(3, 2'000, 0, static_cast<std::uint8_t>(core::OrdStatus::Working),
                                    static_cast<std::uint8_t>(core::ExecType::PartialFill), "OID1"),
                     2'100);
        // Dropcopy (session_id odd) filled to trigger divergence vs primary state.
        write_record(out, make_wire(1, 3'000, 1, static_cast<std::uint8_t>(core::OrdStatus::Filled),
                                    static_cast<std::uint8_t>(core::ExecType::Fill), "OID1", 100, 100),
                     3'100);
    }

    const auto out1 = tmp_root / "out1";
    api::ReplayConfig cfg;
    cfg.input_files = {log_path};
    cfg.output_dir = out1;
    cfg.fast = true;

    const int rc1 = api::run_replay(cfg);
    ASSERT_EQ(rc1, 0);

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

    const auto out2 = tmp_root / "out2";
    api::ReplayConfig cfg_verify = cfg;
    cfg_verify.output_dir = out2;
    cfg_verify.verify_against = out1;

    const int rc2 = api::run_replay(cfg_verify);
    EXPECT_EQ(rc2, 0);
}
