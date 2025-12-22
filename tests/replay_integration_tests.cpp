#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>
#include <cstring>
#include <iterator>

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

void write_record(std::ofstream& out, const core::WireExecEvent& evt) {
    const std::span<const std::byte> payload(reinterpret_cast<const std::byte*>(&evt), sizeof(evt));
    const auto checksum = persist::crc32c(payload);
    std::uint32_t len_le{0};
    std::uint32_t crc_le{0};
    persist::encode_record(payload, checksum, len_le, crc_le);
    out.write(reinterpret_cast<const char*>(&len_le), sizeof(len_le));
    out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    out.write(reinterpret_cast<const char*>(&crc_le), sizeof(crc_le));
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
        // Primary (session_id even) gap between seq 1 and 3 to trigger gap detection.
        write_record(out, make_wire(1, 1'000, 0, static_cast<std::uint8_t>(core::OrdStatus::New),
                                    static_cast<std::uint8_t>(core::ExecType::New), "OID1"));
        write_record(out, make_wire(3, 2'000, 0, static_cast<std::uint8_t>(core::OrdStatus::Working),
                                    static_cast<std::uint8_t>(core::ExecType::PartialFill), "OID1"));
        // Dropcopy (session_id odd) filled to trigger divergence vs primary state.
        write_record(out, make_wire(1, 3'000, 1, static_cast<std::uint8_t>(core::OrdStatus::Filled),
                                    static_cast<std::uint8_t>(core::ExecType::Fill), "OID1", 100, 100));
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
