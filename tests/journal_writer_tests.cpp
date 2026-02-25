// FX-8002: JournalWriter unit tests.

#include <filesystem>
#include <gtest/gtest.h>

#include "persist/journal_writer.hpp"

namespace {

std::filesystem::path temp_path(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

// ── EnqueueBeforeStart_ReturnsFalse ────────────────────────────────────────

TEST(JournalWriterTests, EnqueueBeforeStart_ReturnsFalse) {
    persist::JournalConfig cfg;
    cfg.path              = temp_path("jw_nostart.bin");
    cfg.segment_bytes     = 1u << 20;
    cfg.ring_capacity_pow2 = 64;

    persist::JournalWriter writer(cfg);

    std::byte payload[16]{};
    EXPECT_FALSE(writer.enqueue_record(
        persist::RecordType::ExecEvent, 0, 1, payload, 16));
}

// ── StartStop_Idempotent ───────────────────────────────────────────────────

TEST(JournalWriterTests, StartStop_Idempotent) {
    persist::JournalConfig cfg;
    cfg.path              = temp_path("jw_idempotent.bin");
    cfg.segment_bytes     = 1u << 20;
    cfg.ring_capacity_pow2 = 64;

    persist::JournalWriter writer(cfg);

    EXPECT_TRUE(writer.start());
    EXPECT_TRUE(writer.start()); // second call is idempotent

    writer.stop();
    writer.stop(); // second stop is idempotent
}

// ── RingOverflow_IncrementsDropped_NoBlock ──────────────────────────────────

TEST(JournalWriterTests, RingOverflow_IncrementsDropped_NoBlock) {
    persist::JournalConfig cfg;
    cfg.path              = temp_path("jw_overflow.bin");
    cfg.segment_bytes     = 1u << 20;
    cfg.ring_capacity_pow2 = 8; // tiny ring: 7 usable slots

    persist::JournalWriter writer(cfg);
    ASSERT_TRUE(writer.start());

    // Enqueue rapidly; some must drop because the ring is tiny.
    std::byte payload[4]{};
    int local_dropped = 0;
    for (int i = 0; i < 100; ++i) {
        if (!writer.enqueue_record(
                persist::RecordType::ExecEvent, 0,
                static_cast<std::uint64_t>(i), payload, 4))
            ++local_dropped;
    }

    writer.stop();
    EXPECT_GT(local_dropped, 0);
    EXPECT_GT(writer.stats().dropped, 0u);
}

// ── PayloadTooLargeForRing_Drops ───────────────────────────────────────────

TEST(JournalWriterTests, PayloadTooLargeForRing_Drops) {
    persist::JournalConfig cfg;
    cfg.path              = temp_path("jw_toolarge.bin");
    cfg.segment_bytes     = 1u << 20;
    cfg.ring_capacity_pow2 = 64;

    persist::JournalWriter writer(cfg);
    ASSERT_TRUE(writer.start());

    // Payload exceeds kMaxRingPayload (512).
    std::byte big[1024]{};
    EXPECT_FALSE(writer.enqueue_record(
        persist::RecordType::ExecEvent, 0, 1, big, sizeof(big)));

    writer.stop();
    EXPECT_GE(writer.stats().dropped, 1u);
}

} // namespace
