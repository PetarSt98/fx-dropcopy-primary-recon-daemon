// FX-8002: JournalWriter integration tests.
// Writes real records through the async pipeline and verifies on-disk content
// using persist::decode_record_v1.

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "persist/journal_writer.hpp"
#include "persist/record_format.hpp"

namespace {

class JournalWriterIntegration : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() / "jw_integ";
        std::filesystem::create_directories(test_dir_);
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(test_dir_, ec);
    }
    std::filesystem::path test_dir_;
};

// ── WriteNRecords_ScanValidAndMonotonic ────────────────────────────────────

TEST_F(JournalWriterIntegration, WriteNRecords_ScanValidAndMonotonic) {
    const auto path = test_dir_ / "monotonic.bin";
    constexpr int N = 100;

    std::uint64_t total_bytes_written = 0;
    {
        persist::JournalConfig cfg;
        cfg.path              = path;
        cfg.segment_bytes     = 1u << 20; // 1 MiB
        cfg.ring_capacity_pow2 = 256;

        persist::JournalWriter writer(cfg);
        ASSERT_TRUE(writer.start());

        for (int i = 0; i < N; ++i) {
            std::byte payload[32]{};
            auto seq = static_cast<std::uint64_t>(i + 1);
            std::memcpy(payload, &seq, sizeof(seq));

            ASSERT_TRUE(writer.enqueue_record(
                persist::RecordType::ExecEvent,
                static_cast<std::uint16_t>(i & 0xFFFF),
                seq, payload, 32));
        }

        // Wait for background thread to drain.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (writer.stats().written < static_cast<std::uint64_t>(N) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        writer.stop();
        EXPECT_EQ(writer.stats().written, static_cast<std::uint64_t>(N));
        total_bytes_written = writer.stats().bytes_written;
    }

    // Read only the written portion of the file.
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in.is_open());

    std::vector<std::byte> data(static_cast<std::size_t>(total_bytes_written));
    in.read(reinterpret_cast<char*>(data.data()),
            static_cast<std::streamsize>(data.size()));

    std::size_t   offset    = 0;
    int           count     = 0;
    std::uint64_t last_seq  = 0;

    while (offset < data.size()) {
        auto r = persist::decode_record_v1(data.data() + offset,
                                           data.size() - offset);
        if (r.status != persist::DecodeStatus::Ok) break;

        EXPECT_EQ(r.type, persist::RecordType::ExecEvent);
        EXPECT_GT(r.seqno, last_seq)
            << "Monotonicity violated at record " << count;
        last_seq = r.seqno;
        ++count;
        offset += r.bytes_consumed;
    }

    EXPECT_EQ(count, N);
}

// ── TruncatedTail_IsTolerated ──────────────────────────────────────────────

TEST_F(JournalWriterIntegration, TruncatedTail_IsTolerated) {
    const auto path = test_dir_ / "truncated.bin";
    constexpr int N = 50;

    std::uint64_t total_bytes_written = 0;
    {
        persist::JournalConfig cfg;
        cfg.path              = path;
        cfg.segment_bytes     = 1u << 20;
        cfg.ring_capacity_pow2 = 256;

        persist::JournalWriter writer(cfg);
        ASSERT_TRUE(writer.start());

        for (int i = 0; i < N; ++i) {
            std::byte payload[16]{};
            auto seq = static_cast<std::uint64_t>(i + 1);
            writer.enqueue_record(persist::RecordType::ExecEvent, 0,
                                  seq, payload, 16);
        }

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (writer.stats().written < static_cast<std::uint64_t>(N) &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        writer.stop();
        ASSERT_EQ(writer.stats().written, static_cast<std::uint64_t>(N));
        total_bytes_written = writer.stats().bytes_written;
    }

    // First, trim file to actual data (remove preallocated zeros).
    std::filesystem::resize_file(path, total_bytes_written);

    // Truncate by removing half of the last record's header → partial tail.
    ASSERT_GT(total_bytes_written,
              static_cast<std::uint64_t>(persist::kHeaderSize));
    const auto truncated_size =
        total_bytes_written - (persist::kHeaderSize / 2);
    std::filesystem::resize_file(path, truncated_size);

    // Scan: should find N-1 valid records and stop cleanly.
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(in.is_open());
    const auto fsize = static_cast<std::size_t>(in.tellg());
    in.seekg(0);

    std::vector<std::byte> data(fsize);
    in.read(reinterpret_cast<char*>(data.data()),
            static_cast<std::streamsize>(fsize));

    std::size_t offset      = 0;
    int         valid_count = 0;

    while (offset < data.size()) {
        auto r = persist::decode_record_v1(data.data() + offset,
                                           data.size() - offset);
        if (r.status != persist::DecodeStatus::Ok) break;
        ++valid_count;
        offset += r.bytes_consumed;
    }

    EXPECT_EQ(valid_count, N - 1);
}

} // namespace
