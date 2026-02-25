#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <vector>

#include <gtest/gtest.h>

#include "persist/journal_writer.hpp"
#include "persist/record_format.hpp"

namespace {

using namespace persist;

// Helper: create a temp file path for tests
static std::filesystem::path temp_journal(const std::string& name) {
    return std::filesystem::temp_directory_path() / name;
}

// Helper: read entire file into a vector
static std::vector<std::byte> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) return {};
    const auto size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::vector<std::byte> buf(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

// ── WriteNRecords_ScanValidAndMonotonic ─────────────────────────────────────

TEST(JournalWriterIntegration, WriteNRecords_ScanValidAndMonotonic) {
    const auto path = temp_journal("jw_integ_write_n.bin");
    constexpr int N = 50;

    JournalConfig cfg;
    cfg.path = path;
    cfg.segment_bytes = 1 << 20; // 1 MiB
    cfg.ring_capacity_pow2 = 256;
    cfg.consumer_sleep_ns = 100;

    {
        JournalWriter writer(cfg);
        ASSERT_TRUE(writer.start());

        // Enqueue N records with increasing seqno
        for (int i = 0; i < N; ++i) {
            std::byte payload[16];
            std::memset(payload, static_cast<int>(i & 0xFF), sizeof(payload));
            bool ok = writer.enqueue_record(
                RecordType::ExecEvent,
                static_cast<std::uint16_t>(i),
                static_cast<std::uint64_t>(i + 1), // seqno 1..N
                payload, sizeof(payload));
            ASSERT_TRUE(ok) << "Failed to enqueue record " << i;
        }

        // Wait for all records to be written
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (writer.stats().written < static_cast<std::uint64_t>(N) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        writer.stop();
        EXPECT_EQ(writer.stats().written, static_cast<std::uint64_t>(N));
        EXPECT_EQ(writer.stats().dropped, 0u);
        EXPECT_EQ(writer.stats().io_errors, 0u);
    }

    // Read back and scan
    auto file_data = read_file(path);
    ASSERT_FALSE(file_data.empty());

    // The file was preallocated to segment_bytes — actual data is at the beginning.
    // scan_records will stop at the first non-valid record (which is zeros from preallocation).
    auto scan = scan_records(file_data.data(), file_data.size());
    EXPECT_EQ(scan.records_ok, static_cast<std::size_t>(N));
    EXPECT_EQ(scan.records_bad, 0u);

    // Verify seqno monotonicity by decoding each record
    std::size_t offset = 0;
    std::uint64_t prev_seqno = 0;
    for (int i = 0; i < N; ++i) {
        DecodedRecordView view{};
        auto st = decode_record_v1(file_data.data() + offset,
                                   file_data.size() - offset, view);
        ASSERT_EQ(st, DecodeStatus::Ok) << "Failed to decode record " << i;
        EXPECT_GT(view.seqno, prev_seqno) << "Seqno not monotonically increasing at record " << i;
        EXPECT_EQ(view.seqno, static_cast<std::uint64_t>(i + 1));
        EXPECT_EQ(view.type, RecordType::ExecEvent);
        EXPECT_EQ(view.flags, static_cast<std::uint16_t>(i));
        EXPECT_EQ(view.length, 16u);
        prev_seqno = view.seqno;
        offset += view.total_bytes;
    }

    std::filesystem::remove(path);
}

// ── TruncatedTail_IsTolerated ───────────────────────────────────────────────

TEST(JournalWriterIntegration, TruncatedTail_IsTolerated) {
    const auto path = temp_journal("jw_integ_truncated.bin");
    constexpr int N = 20;

    JournalConfig cfg;
    cfg.path = path;
    cfg.segment_bytes = 1 << 20;
    cfg.ring_capacity_pow2 = 256;
    cfg.consumer_sleep_ns = 100;

    {
        JournalWriter writer(cfg);
        ASSERT_TRUE(writer.start());

        for (int i = 0; i < N; ++i) {
            std::byte payload[8];
            std::memset(payload, static_cast<int>(i & 0xFF), sizeof(payload));
            ASSERT_TRUE(writer.enqueue_record(
                RecordType::DivergenceEvent,
                0,
                static_cast<std::uint64_t>(i + 1),
                payload, sizeof(payload)));
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (writer.stats().written < static_cast<std::uint64_t>(N) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        writer.stop();
        ASSERT_EQ(writer.stats().written, static_cast<std::uint64_t>(N));
    }

    // Figure out actual data size (N records of header + 8 payload + 0 padding = 32 each)
    const std::size_t record_size = kHeaderSize + 8; // 8 payload, align8(8)=8 → 0 padding
    const std::size_t data_size = static_cast<std::size_t>(N) * record_size;

    // Truncate by removing a partial amount from the last record
    // Simulate crash: cut off some bytes so last record is partial
    const std::size_t truncated_size = data_size - (kHeaderSize / 2);
    std::filesystem::resize_file(path, truncated_size);

    auto file_data = read_file(path);
    ASSERT_EQ(file_data.size(), truncated_size);

    // Scan should recover N-1 records and stop at the partial tail
    auto scan = scan_records(file_data.data(), file_data.size());
    EXPECT_EQ(scan.records_ok, static_cast<std::size_t>(N - 1));
    EXPECT_EQ(scan.records_bad, 0u); // partial tail is NeedMoreData, not "bad"

    std::filesystem::remove(path);
}

// ── SegmentCapacityExceeded_SetsDegradedIO ──────────────────────────────────

TEST(JournalWriterIntegration, SegmentCapacityExceeded_SetsDegradedIO) {
    const auto path = temp_journal("jw_integ_segment_cap.bin");

    JournalConfig cfg;
    cfg.path = path;
    cfg.segment_bytes = 256; // tiny segment — will fill fast
    cfg.ring_capacity_pow2 = 64;
    cfg.consumer_sleep_ns = 100;

    JournalWriter writer(cfg);
    ASSERT_TRUE(writer.start());

    // Enqueue enough records to exceed segment capacity
    for (int i = 0; i < 30; ++i) {
        std::byte payload[16];
        std::memset(payload, 0xAB, sizeof(payload));
        writer.enqueue_record(
            RecordType::ExecEvent, 0,
            static_cast<std::uint64_t>(i + 1),
            payload, sizeof(payload));
    }

    // Wait for background thread to process
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!writer.stats().degraded_io &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    writer.stop();

    auto s = writer.stats();
    EXPECT_TRUE(s.degraded_io);
    EXPECT_GT(s.io_errors, 0u);
    // Some records should have been written before capacity exceeded
    EXPECT_GT(s.written, 0u);

    std::filesystem::remove(path);
}

} // namespace
