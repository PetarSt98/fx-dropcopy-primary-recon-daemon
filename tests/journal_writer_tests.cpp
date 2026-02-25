#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <chrono>

#include <gtest/gtest.h>

#include "persist/journal_writer.hpp"

namespace {

using namespace persist;

// Helper: create a temp file path for tests
static std::filesystem::path temp_journal(const std::string& name) {
    return std::filesystem::temp_directory_path() / name;
}

// Helper: small payload
static const std::byte kHello[] = {
    std::byte{'H'}, std::byte{'e'}, std::byte{'l'},
    std::byte{'l'}, std::byte{'o'}};

// ── EnqueueBeforeStart_ReturnsFalse ─────────────────────────────────────────

TEST(JournalWriterUnit, EnqueueBeforeStart_ReturnsFalse) {
    JournalConfig cfg;
    cfg.path = temp_journal("jw_enqueue_before_start.bin");
    cfg.segment_bytes = 4096;
    cfg.ring_capacity_pow2 = 16;

    JournalWriter writer(cfg);
    // Writer not started — enqueue must return false
    EXPECT_FALSE(writer.enqueue_record(
        RecordType::ExecEvent, 0, 1, kHello, 5));

    // Stats should show nothing enqueued
    auto s = writer.stats();
    EXPECT_EQ(s.enqueued, 0u);
    EXPECT_EQ(s.dropped, 0u);
}

// ── StartStop_Idempotent ────────────────────────────────────────────────────

TEST(JournalWriterUnit, StartStop_Idempotent) {
    JournalConfig cfg;
    cfg.path = temp_journal("jw_start_stop.bin");
    cfg.segment_bytes = 4096;
    cfg.ring_capacity_pow2 = 16;

    JournalWriter writer(cfg);

    // Start twice should both succeed
    EXPECT_TRUE(writer.start());
    EXPECT_TRUE(writer.start());

    // Stop twice should not crash
    writer.stop();
    writer.stop();

    // Cleanup
    std::filesystem::remove(cfg.path);
}

// ── RingOverflow_IncrementsDropped_NoBlock ───────────────────────────────────

TEST(JournalWriterUnit, RingOverflow_IncrementsDropped_NoBlock) {
    JournalConfig cfg;
    cfg.path = temp_journal("jw_ring_overflow.bin");
    cfg.segment_bytes = 1 << 20; // 1 MiB
    cfg.ring_capacity_pow2 = 8;  // very small ring (8 slots, usable = 7)
    cfg.consumer_sleep_ns = 100'000'000; // very slow consumer (100ms) to force overflow

    JournalWriter writer(cfg);
    ASSERT_TRUE(writer.start());

    // Enqueue more records than ring capacity (some should drop)
    std::size_t accepted = 0;
    std::size_t rejected = 0;
    for (int i = 0; i < 100; ++i) {
        bool ok = writer.enqueue_record(
            RecordType::ExecEvent, 0, static_cast<std::uint64_t>(i), kHello, 5);
        if (ok) ++accepted; else ++rejected;
    }

    // Some should have been dropped (ring full)
    EXPECT_GT(rejected, 0u);

    // This should not hang — the test itself is a timeout check
    writer.stop();

    auto s = writer.stats();
    EXPECT_EQ(s.enqueued, accepted);
    EXPECT_GT(s.dropped, 0u);

    std::filesystem::remove(cfg.path);
}

// ── PayloadTooLargeForRing_Drops ────────────────────────────────────────────

TEST(JournalWriterUnit, PayloadTooLargeForRing_Drops) {
    JournalConfig cfg;
    cfg.path = temp_journal("jw_payload_too_large.bin");
    cfg.segment_bytes = 1 << 20;
    cfg.ring_capacity_pow2 = 16;

    JournalWriter writer(cfg);
    ASSERT_TRUE(writer.start());

    // Payload larger than kMaxRingPayloadBytes should be dropped
    std::vector<std::byte> big_payload(kMaxRingPayloadBytes + 1, std::byte{0xAB});
    bool ok = writer.enqueue_record(
        RecordType::ExecEvent, 0, 1,
        big_payload.data(), static_cast<std::uint32_t>(big_payload.size()));
    EXPECT_FALSE(ok);

    auto s = writer.stats();
    EXPECT_EQ(s.dropped, 1u);
    EXPECT_EQ(s.enqueued, 0u);

    writer.stop();
    std::filesystem::remove(cfg.path);
}

// ── EmptyPayload_Accepted ───────────────────────────────────────────────────

TEST(JournalWriterUnit, EmptyPayload_Accepted) {
    JournalConfig cfg;
    cfg.path = temp_journal("jw_empty_payload.bin");
    cfg.segment_bytes = 4096;
    cfg.ring_capacity_pow2 = 16;

    JournalWriter writer(cfg);
    ASSERT_TRUE(writer.start());

    bool ok = writer.enqueue_record(RecordType::SnapshotMeta, 0, 42, nullptr, 0);
    EXPECT_TRUE(ok);

    // Wait for background thread to drain
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (writer.stats().written < 1 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    writer.stop();
    EXPECT_EQ(writer.stats().written, 1u);

    std::filesystem::remove(cfg.path);
}

} // namespace
