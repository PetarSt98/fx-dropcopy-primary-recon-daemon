#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "persist/journal_writer.hpp"

namespace {

using namespace persist;

// Temp directory helper — creates per-test unique path
class JournalWriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmp_dir_ = std::filesystem::temp_directory_path() / "jw_unit_test";
        std::filesystem::create_directories(tmp_dir_);
        journal_path_ = tmp_dir_ / "test.journal";
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(tmp_dir_, ec);
    }

    JournalConfig make_config(std::size_t ring_cap = 1u << 10,
                              std::uint64_t seg    = 1ULL << 20) {
        JournalConfig c;
        c.path               = journal_path_;
        c.segment_bytes      = seg;
        c.ring_capacity_pow2 = ring_cap;
        c.max_payload_bytes  = kJournalMaxPayload;
        c.idle_sleep_ns      = 500;
        return c;
    }

    std::filesystem::path tmp_dir_;
    std::filesystem::path journal_path_;
};

// ── EnqueueBeforeStart_ReturnsFalse ──────────────────────────────────────────

TEST_F(JournalWriterTest, EnqueueBeforeStart_ReturnsFalse) {
    JournalWriter w(make_config());
    std::byte payload[8] = {};
    EXPECT_FALSE(w.enqueue_record(RecordType::ExecEvent, 0, 1, payload, 8));
    auto s = w.stats();
    EXPECT_EQ(s.enqueued, 0u);
}

// ── StartStop_Idempotent ─────────────────────────────────────────────────────

TEST_F(JournalWriterTest, StartStop_Idempotent) {
    JournalWriter w(make_config());
    EXPECT_TRUE(w.start());
    EXPECT_FALSE(w.start());   // second start returns false
    w.stop();
    w.stop();                   // second stop is safe (no crash)
}

// ── PayloadTooLarge_Drops ────────────────────────────────────────────────────

TEST_F(JournalWriterTest, PayloadTooLarge_Drops) {
    JournalWriter w(make_config());
    ASSERT_TRUE(w.start());

    std::vector<std::byte> big(kJournalMaxPayload + 1, std::byte{0xAB});
    EXPECT_FALSE(w.enqueue_record(RecordType::ExecEvent, 0, 1,
                                  big.data(),
                                  static_cast<std::uint32_t>(big.size())));

    w.stop();
    auto s = w.stats();
    EXPECT_EQ(s.dropped, 1u);
    EXPECT_EQ(s.enqueued, 0u);
}

// ── RingOverflow_Drops ───────────────────────────────────────────────────────

TEST_F(JournalWriterTest, RingOverflow_Drops) {
    // Tiny ring (4 slots ⇒ 3 usable) to guarantee overflow
    auto cfg = make_config(/*ring_cap=*/4);
    cfg.idle_sleep_ns = 100'000'000; // 100 ms — I/O thread sleeps long enough
    JournalWriter w(cfg);
    ASSERT_TRUE(w.start());

    std::byte payload[16] = {};
    // Burst-enqueue far more than ring can hold
    for (int i = 0; i < 100; ++i) {
        w.enqueue_record(RecordType::ExecEvent, 0,
                         static_cast<std::uint64_t>(i), payload, 16);
    }

    // Give I/O thread time to drain what it can
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    w.stop();

    auto s = w.stats();
    EXPECT_GT(s.dropped, 0u);
    EXPECT_GT(s.enqueued, 0u);
    // Total enqueued + dropped should equal 100
    EXPECT_EQ(s.enqueued + s.dropped, 100u);
}

} // namespace
