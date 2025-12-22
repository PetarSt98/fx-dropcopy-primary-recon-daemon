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
        write_header(out);
        auto evt1 = make_event(1, 100);
        auto evt2 = make_event(2, 200);
        write_record(out, evt1, 101);
        write_record(out, evt2, 202);
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
    EXPECT_EQ(ts, 101u);

    res = reader.next(evt, ts);
    ASSERT_EQ(res.status, persist::WireLogReadStatus::Ok);
    EXPECT_EQ(evt.seq_num, 2u);
    EXPECT_EQ(ts, 202u);

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
        write_header(out);
        auto evt = make_event(5, 500);
        write_record(out, evt, 501);
        auto evt2 = make_event(6, 600);
        write_record(out, evt2, 601);
    }
    // Flip one byte in the payload.
    {
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(persist::wire_log_header_size + sizeof(std::uint32_t) + sizeof(std::uint64_t)); // after length+ts
        char byte{};
        f.read(&byte, 1);
        byte ^= 0x01;
        f.seekp(persist::wire_log_header_size + sizeof(std::uint32_t) + sizeof(std::uint64_t));
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

    res = reader.next(evt, ts);
    ASSERT_EQ(res.status, persist::WireLogReadStatus::Ok);
    EXPECT_EQ(evt.seq_num, 6u);
}

TEST(WireLogReader, HandlesTruncatedTail) {
    const auto path = make_tmp_log("truncated.bin");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        write_header(out);
        auto evt = make_event(7, 700);
        write_record(out, evt, 701);
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
        write_header(out);
        write_record(out, make_event(10, 1000), 5000);
        write_record(out, make_event(11, 2000), 6000);
        write_record(out, make_event(12, 3000), 7000);
    }
    persist::WireLogReaderOptions opts;
    opts.files = {path};
    opts.use_time_window = true;
    opts.window_start_ns = 5500;
    opts.window_end_ns = 6500;
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

TEST(WireLogReader, DirectoryScanOrdersByTimestamp) {
    const auto dir = std::filesystem::temp_directory_path() / "wire_log_reader_tests_order";
    std::filesystem::create_directories(dir);
    const auto p1 = dir / "wire_capture_20240101_000001_seq001.bin";
    const auto p2 = dir / "wire_capture_20240101_000002_seq000.bin";
    {
        std::ofstream out1(p2, std::ios::binary | std::ios::trunc);
        write_header(out1);
        write_record(out1, make_event(20, 20), 20);
        std::ofstream out2(p1, std::ios::binary | std::ios::trunc);
        write_header(out2);
        write_record(out2, make_event(21, 21), 21);
    }
    persist::WireLogReaderOptions opts;
    opts.directory = dir;
    persist::WireLogReader reader(opts);
    ASSERT_TRUE(reader.open());
    core::WireExecEvent evt{};
    std::uint64_t ts{0};
    auto res = reader.next(evt, ts);
    ASSERT_EQ(res.status, persist::WireLogReadStatus::Ok);
    EXPECT_EQ(evt.seq_num, 21u); // timestamp order
    res = reader.next(evt, ts);
    ASSERT_EQ(res.status, persist::WireLogReadStatus::Ok);
    EXPECT_EQ(evt.seq_num, 20u);
}

TEST(WireLogReader, RejectsInvalidHeader) {
    const auto path = make_tmp_log("bad_header.bin");
    persist::WireLogHeaderFields header{};
    std::array<std::byte, persist::wire_log_header_size> bytes{};
    persist::encode_header(header, bytes);
    bytes[0] = std::byte{0x00}; // break magic
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    persist::WireLogReaderOptions opts;
    opts.files = {path};
    persist::WireLogReader reader(std::move(opts));
    EXPECT_FALSE(reader.open());
    EXPECT_EQ(reader.stats().header_invalid, 1u);
}

TEST(WireLogReader, RejectsWrongPayloadLength) {
    const auto path = make_tmp_log("bad_length.bin");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        write_header(out);
        std::array<std::byte, 10> small{};
        persist::RecordFields fields{};
        persist::encode_record(small, 900, fields);
        out.write(reinterpret_cast<const char*>(fields.length_le.data()), static_cast<std::streamsize>(fields.length_le.size()));
        out.write(reinterpret_cast<const char*>(fields.capture_ts_le.data()), static_cast<std::streamsize>(fields.capture_ts_le.size()));
        out.write(reinterpret_cast<const char*>(small.data()), static_cast<std::streamsize>(small.size()));
        out.write(reinterpret_cast<const char*>(fields.checksum_le.data()), static_cast<std::streamsize>(fields.checksum_le.size()));

        write_record(out, make_event(99, 9'000), 9'001);
    }
    persist::WireLogReaderOptions opts;
    opts.files = {path};
    persist::WireLogReader reader(std::move(opts));
    ASSERT_TRUE(reader.open());
    core::WireExecEvent evt{};
    std::uint64_t ts{0};
    auto res = reader.next(evt, ts);
    EXPECT_EQ(res.status, persist::WireLogReadStatus::InvalidLength);
    EXPECT_EQ(reader.stats().bad_length, 1u);

    res = reader.next(evt, ts);
    ASSERT_EQ(res.status, persist::WireLogReadStatus::Ok);
    EXPECT_EQ(evt.seq_num, 99u);
}
