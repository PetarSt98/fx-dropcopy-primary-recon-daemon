#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
#include <string>
#include <array>

#include "core/wire_exec_event.hpp"
#include "persist/wire_log_format.hpp"
#include "persist/wire_log_reader.hpp"

namespace {

std::filesystem::path make_tmp_log(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / "wire_log_reader_tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

void write_record(std::ofstream& out, const core::WireExecEvent& evt) {
    std::array<std::byte, persist::wire_exec_event_serialized_size> payload{};
    ASSERT_TRUE(persist::serialize_wire_exec_event(evt, payload.data()));
    std::uint32_t len_le{0};
    std::uint32_t crc_le{0};
    persist::encode_record(payload, len_le, crc_le);
    out.write(reinterpret_cast<const char*>(&len_le), sizeof(len_le));
    out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    out.write(reinterpret_cast<const char*>(&crc_le), sizeof(crc_le));
}

core::WireExecEvent make_event(std::uint64_t seq, std::uint64_t ts) {
    core::WireExecEvent evt{};
    evt.seq_num = seq;
    evt.sending_time = ts;
    evt.transact_time = ts;
    evt.exec_type = static_cast<std::uint8_t>(seq & 0xFFu);
    evt.ord_status = static_cast<std::uint8_t>((seq + 1) & 0xFFu);
    const std::string id = "id" + std::to_string(seq);
    evt.exec_id_len = static_cast<std::uint8_t>(id.size());
    std::memcpy(evt.exec_id, id.data(), id.size());
    return evt;
}

} // namespace

TEST(WireLogReader, RoundTripSingleFile) {
    const auto path = make_tmp_log("single.bin");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        auto evt1 = make_event(1, 100);
        auto evt2 = make_event(2, 200);
        write_record(out, evt1);
        write_record(out, evt2);
    }
    persist::WireLogReaderOptions opts;
    opts.files = {path};
    persist::WireLogReader reader(std::move(opts));
    ASSERT_TRUE(reader.open());
    core::WireExecEvent evt{};
    std::uint64_t ts{0};
    auto res = reader.next(evt, ts);
    ASSERT_EQ(res.status, persist::WireLogReadStatus::Ok);
    EXPECT_EQ(evt.seq_num, 1u);
    EXPECT_EQ(ts, 100u);

    res = reader.next(evt, ts);
    ASSERT_EQ(res.status, persist::WireLogReadStatus::Ok);
    EXPECT_EQ(evt.seq_num, 2u);
    EXPECT_EQ(ts, 200u);

    res = reader.next(evt, ts);
    EXPECT_EQ(res.status, persist::WireLogReadStatus::EndOfStream);
    const auto& stats = reader.stats();
    EXPECT_EQ(stats.records_ok, 2u);
    EXPECT_EQ(stats.checksum_failures, 0u);
    EXPECT_EQ(stats.truncated_tail, 0u);
}

TEST(WireLogReader, DetectsChecksumFailure) {
    const auto path = make_tmp_log("checksum.bin");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        auto evt = make_event(5, 500);
        write_record(out, evt);
    }
    // Flip one byte in the payload.
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(sizeof(std::uint32_t)); // after length
        char byte{};
        f.read(&byte, 1);
        byte ^= 0x01;
        f.seekp(sizeof(std::uint32_t));
        f.write(&byte, 1);
    }
    persist::WireLogReaderOptions opts;
    opts.files = {path};
    persist::WireLogReader reader(std::move(opts));
    ASSERT_TRUE(reader.open());
    core::WireExecEvent evt{};
    std::uint64_t ts{0};
    auto res = reader.next(evt, ts);
    EXPECT_EQ(res.status, persist::WireLogReadStatus::ChecksumMismatch);
    EXPECT_EQ(reader.stats().checksum_failures, 1u);
}

TEST(WireLogReader, HandlesTruncatedTail) {
    const auto path = make_tmp_log("truncated.bin");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        auto evt = make_event(7, 700);
        write_record(out, evt);
        std::uint32_t bogus_len = persist::to_le32(64);
        out.write(reinterpret_cast<const char*>(&bogus_len), sizeof(bogus_len)); // incomplete frame
    }
    persist::WireLogReaderOptions opts;
    opts.files = {path};
    persist::WireLogReader reader(std::move(opts));
    ASSERT_TRUE(reader.open());
    core::WireExecEvent evt{};
    std::uint64_t ts{0};
    auto res = reader.next(evt, ts);
    ASSERT_EQ(res.status, persist::WireLogReadStatus::Ok);
    res = reader.next(evt, ts);
    EXPECT_EQ(res.status, persist::WireLogReadStatus::Truncated);
    EXPECT_EQ(reader.stats().truncated_tail, 1u);
}

TEST(WireLogReader, TimeWindowFiltering) {
    const auto path = make_tmp_log("window.bin");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        write_record(out, make_event(10, 1000));
        write_record(out, make_event(11, 2000));
        write_record(out, make_event(12, 3000));
    }
    persist::WireLogReaderOptions opts;
    opts.files = {path};
    opts.use_time_window = true;
    opts.window_start_ns = 1500;
    opts.window_end_ns = 2500;
    persist::WireLogReader reader(std::move(opts));
    ASSERT_TRUE(reader.open());
    core::WireExecEvent evt{};
    std::uint64_t ts{0};

    auto res = reader.next(evt, ts);
    ASSERT_EQ(res.status, persist::WireLogReadStatus::Ok);
    EXPECT_EQ(evt.seq_num, 11u);
    EXPECT_EQ(ts, 2000u);

    res = reader.next(evt, ts);
    EXPECT_EQ(res.status, persist::WireLogReadStatus::EndOfStream);
    EXPECT_EQ(reader.stats().filtered_out, 2u);
}

TEST(WireLogReader, DirectoryScanOrdersLexically) {
    const auto dir = std::filesystem::temp_directory_path() / "wire_log_reader_tests_order";
    std::filesystem::create_directories(dir);
    const auto p1 = dir / "wire_capture_001.bin";
    const auto p2 = dir / "wire_capture_010.bin";
    {
        std::ofstream out1(p2, std::ios::binary | std::ios::trunc);
        write_record(out1, make_event(20, 20));
        std::ofstream out2(p1, std::ios::binary | std::ios::trunc);
        write_record(out2, make_event(21, 21));
    }
    persist::WireLogReaderOptions opts;
    opts.directory = dir;
    persist::WireLogReader reader(opts);
    ASSERT_TRUE(reader.open());
    core::WireExecEvent evt{};
    std::uint64_t ts{0};
    auto res = reader.next(evt, ts);
    ASSERT_EQ(res.status, persist::WireLogReadStatus::Ok);
    EXPECT_EQ(evt.seq_num, 21u); // lexical order: 001 before 010
    res = reader.next(evt, ts);
    ASSERT_EQ(res.status, persist::WireLogReadStatus::Ok);
    EXPECT_EQ(evt.seq_num, 20u);
}
