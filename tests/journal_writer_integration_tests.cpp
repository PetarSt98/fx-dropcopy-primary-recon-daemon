#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "persist/journal_writer.hpp"
#include "persist/record_format.hpp"

namespace {

using namespace persist;

// ---------------------------------------------------------------------------
// Helper: read entire file into a byte vector
// ---------------------------------------------------------------------------
static std::vector<std::byte> read_file(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::binary | std::ios::ate);
    if (!ifs) return {};
    auto sz = ifs.tellg();
    ifs.seekg(0);
    std::vector<std::byte> buf(static_cast<std::size_t>(sz));
    ifs.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------
class JournalWriterIntegration : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = std::filesystem::temp_directory_path() / "jw_integ_test";
        std::filesystem::create_directories(tmp_dir_);
        journal_path_ = tmp_dir_ / "test.journal";
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(tmp_dir_, ec);
    }

    JournalConfig make_config(std::uint64_t seg = 1ULL << 20) {
        JournalConfig c;
        c.path               = journal_path_;
        c.segment_bytes      = seg;
        c.ring_capacity_pow2 = 1u << 10;
        c.max_payload_bytes  = kJournalMaxPayload;
        c.idle_sleep_ns      = 500;
        return c;
    }

    std::filesystem::path tmp_dir_;
    std::filesystem::path journal_path_;
};

// ── WriteNRecords_ScanValidAndMonotonic ──────────────────────────────────────

TEST_F(JournalWriterIntegration, WriteNRecords_ScanValidAndMonotonic) {
    constexpr int N = 200;
    const std::byte sample_payload[] = {
        std::byte{'T'}, std::byte{'E'}, std::byte{'S'}, std::byte{'T'}};

    JournalWriter w(make_config());
    ASSERT_TRUE(w.start());

    for (int i = 1; i <= N; ++i) {
        bool ok = w.enqueue_record(
            RecordType::ExecEvent, 0,
            static_cast<std::uint64_t>(i),
            sample_payload, sizeof(sample_payload));
        ASSERT_TRUE(ok) << "enqueue failed at i=" << i;
    }

    // Allow I/O thread to drain
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    w.stop();

    auto s = w.stats();
    EXPECT_EQ(s.enqueued, static_cast<std::uint64_t>(N));
    EXPECT_EQ(s.written,  static_cast<std::uint64_t>(N));
    EXPECT_EQ(s.dropped,  0u);
    EXPECT_FALSE(s.degraded_io);

    // Read file and scan with FX-8001
    auto data = read_file(journal_path_);
    ASSERT_FALSE(data.empty());

    auto scan = scan_records(data.data(), data.size());
    EXPECT_EQ(scan.records_ok, static_cast<std::size_t>(N));
    EXPECT_EQ(scan.records_bad, 0u);

    // Verify monotonic seqno
    std::size_t offset = 0;
    std::uint64_t prev_seqno = 0;
    for (int i = 0; i < N; ++i) {
        DecodedRecordView view{};
        auto st = decode_record_v1(data.data() + offset,
                                   data.size() - offset, view);
        ASSERT_EQ(st, DecodeStatus::Ok) << "decode failed at record " << i;
        EXPECT_GT(view.seqno, prev_seqno)
            << "seqno not monotonic at record " << i;
        prev_seqno = view.seqno;
        offset += view.total_bytes;
    }
}

// ── TruncatedTail_Tolerated ─────────────────────────────────────────────────

TEST_F(JournalWriterIntegration, TruncatedTail_Tolerated) {
    constexpr int N = 50;
    const std::byte payload[] = {std::byte{'A'}, std::byte{'B'}};

    JournalWriter w(make_config());
    ASSERT_TRUE(w.start());

    for (int i = 1; i <= N; ++i) {
        ASSERT_TRUE(w.enqueue_record(
            RecordType::DivergenceEvent, 0,
            static_cast<std::uint64_t>(i), payload, sizeof(payload)));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    w.stop();

    // Verify all N records were written
    auto s = w.stats();
    ASSERT_EQ(s.written, static_cast<std::uint64_t>(N));

    // Truncate file — remove some bytes from the end to corrupt last record
    auto file_size = std::filesystem::file_size(journal_path_);
    ASSERT_GT(file_size, 10u);
    std::filesystem::resize_file(journal_path_, file_size - 5);

    // Re-read and scan
    auto data = read_file(journal_path_);
    auto scan = scan_records(data.data(), data.size());

    // Should have N-1 valid records and 0 bad (partial tail = NeedMoreData, not bad)
    EXPECT_EQ(scan.records_ok, static_cast<std::size_t>(N - 1));
    EXPECT_EQ(scan.records_bad, 0u);
}

// ── SegmentCapacityExceeded_SetsDegradedIO ──────────────────────────────────

TEST_F(JournalWriterIntegration, SegmentCapacityExceeded_SetsDegradedIO) {
    // Very small segment — only enough for a few records
    // Each record: header(24) + payload(4) + padding(4) = 32 bytes
    // Segment of 256 bytes → ~8 records max
    constexpr std::uint64_t tiny_segment = 256;
    auto cfg = make_config(tiny_segment);
    JournalWriter w(cfg);
    ASSERT_TRUE(w.start());

    const std::byte payload[] = {
        std::byte{'X'}, std::byte{'Y'}, std::byte{'Z'}, std::byte{'W'}};

    for (int i = 1; i <= 100; ++i) {
        w.enqueue_record(RecordType::ExecEvent, 0,
                         static_cast<std::uint64_t>(i),
                         payload, sizeof(payload));
    }

    // Allow I/O thread to process everything
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    w.stop();

    auto s = w.stats();
    EXPECT_TRUE(s.degraded_io);
    EXPECT_GT(s.io_errors, 0u);
    EXPECT_GT(s.written, 0u);    // some records were written before degraded
    EXPECT_GT(s.enqueued, 0u);
}

} // namespace
