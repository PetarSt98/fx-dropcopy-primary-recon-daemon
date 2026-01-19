#include <gtest/gtest.h>

#include "util/wheel_timer.hpp"
#include "util/tsc_calibration.hpp"

namespace {

using WheelTimer = util::WheelTimer;

class WheelTimerTest : public ::testing::Test {
protected:
    // Start at time 0 for simplicity
    WheelTimer timer_{0};

    // Helper: convert milliseconds to TSC cycles (matching timer wheel's tick granularity)
    static std::uint64_t ms_to_tsc(std::uint64_t ms) {
        return util::ns_to_tsc(ms * 1'000'000ULL);  // ms -> ns -> tsc
    }
};

// ScheduleAndExpire - Schedule entry, advance time, verify callback invoked
TEST_F(WheelTimerTest, ScheduleAndExpire) {
    const core::OrderKey key = 12345;
    const std::uint32_t gen = 1;
    const std::uint64_t deadline = ms_to_tsc(5);  // 5ms from start

    ASSERT_TRUE(timer_.schedule(key, gen, deadline));
    EXPECT_EQ(timer_.total_pending(), 1u);
    EXPECT_EQ(timer_.stats().scheduled, 1u);

    // Track callback invocations
    core::OrderKey callback_key = 0;
    std::uint32_t callback_gen = 0;
    int callback_count = 0;

    auto on_expired = [&](core::OrderKey k, std::uint32_t g) {
        callback_key = k;
        callback_gen = g;
        ++callback_count;
    };

    // Poll before deadline - no callback (entry is in tick 5, polling at 4ms advances to tick 4)
    timer_.poll_expired(ms_to_tsc(4), on_expired);
    EXPECT_EQ(callback_count, 0);
    EXPECT_EQ(timer_.total_pending(), 1u);

    // Poll after deadline - callback invoked (polling at 6ms processes tick 5)
    timer_.poll_expired(ms_to_tsc(6), on_expired);
    EXPECT_EQ(callback_count, 1);
    EXPECT_EQ(callback_key, key);
    EXPECT_EQ(callback_gen, gen);
    EXPECT_EQ(timer_.total_pending(), 0u);
    EXPECT_EQ(timer_.stats().expired, 1u);
}

// ScheduleMultipleSameBucket - Multiple entries in same bucket all expire together
TEST_F(WheelTimerTest, ScheduleMultipleSameBucket) {
    const std::uint64_t deadline = ms_to_tsc(10);

    // Schedule 3 entries with same deadline
    ASSERT_TRUE(timer_.schedule(100, 1, deadline));
    ASSERT_TRUE(timer_.schedule(200, 2, deadline));
    ASSERT_TRUE(timer_.schedule(300, 3, deadline));
    EXPECT_EQ(timer_.total_pending(), 3u);

    int callback_count = 0;
    auto on_expired = [&](core::OrderKey, std::uint32_t) {
        ++callback_count;
    };

    // Poll after deadline - all 3 should expire
    timer_.poll_expired(ms_to_tsc(11), on_expired);
    EXPECT_EQ(callback_count, 3);
    EXPECT_EQ(timer_.total_pending(), 0u);
    EXPECT_EQ(timer_.stats().expired, 3u);
}

// ScheduleDifferentBuckets - Entries in different buckets expire at correct times
TEST_F(WheelTimerTest, ScheduleDifferentBuckets) {
    // Schedule entries at different times
    ASSERT_TRUE(timer_.schedule(100, 1, ms_to_tsc(5)));   // expires at 5ms
    ASSERT_TRUE(timer_.schedule(200, 2, ms_to_tsc(10)));  // expires at 10ms
    ASSERT_TRUE(timer_.schedule(300, 3, ms_to_tsc(15)));  // expires at 15ms
    EXPECT_EQ(timer_.total_pending(), 3u);

    std::vector<core::OrderKey> expired_keys;
    auto on_expired = [&](core::OrderKey k, std::uint32_t) {
        expired_keys.push_back(k);
    };

    // Poll at 6ms - only first should expire
    timer_.poll_expired(ms_to_tsc(6), on_expired);
    EXPECT_EQ(expired_keys.size(), 1u);
    EXPECT_EQ(expired_keys[0], 100u);
    EXPECT_EQ(timer_.total_pending(), 2u);

    // Poll at 11ms - second should expire
    timer_.poll_expired(ms_to_tsc(11), on_expired);
    EXPECT_EQ(expired_keys.size(), 2u);
    EXPECT_EQ(expired_keys[1], 200u);
    EXPECT_EQ(timer_.total_pending(), 1u);

    // Poll at 16ms - third should expire
    timer_.poll_expired(ms_to_tsc(16), on_expired);
    EXPECT_EQ(expired_keys.size(), 3u);
    EXPECT_EQ(expired_keys[2], 300u);
    EXPECT_EQ(timer_.total_pending(), 0u);
}

// GenerationPassedToCallback - Wheel passes generation to callback unchanged
TEST_F(WheelTimerTest, GenerationPassedToCallback) {
    const std::uint32_t expected_gen = 42;
    ASSERT_TRUE(timer_.schedule(100, expected_gen, ms_to_tsc(5)));

    std::uint32_t received_gen = 0;
    auto on_expired = [&](core::OrderKey, std::uint32_t g) {
        received_gen = g;
    };

    timer_.poll_expired(ms_to_tsc(6), on_expired);
    EXPECT_EQ(received_gen, expected_gen);
}

// BucketOverflowReturnsFalse - Schedule returns false when bucket full
TEST_F(WheelTimerTest, BucketOverflowReturnsFalse) {
    const std::uint64_t deadline = ms_to_tsc(5);

    // Fill a bucket to capacity
    for (std::size_t i = 0; i < WheelTimer::BUCKET_CAPACITY; ++i) {
        ASSERT_TRUE(timer_.schedule(i, 1, deadline)) << "Failed at index " << i;
    }
    EXPECT_EQ(timer_.total_pending(), WheelTimer::BUCKET_CAPACITY);

    // Next schedule should fail
    EXPECT_FALSE(timer_.schedule(9999, 1, deadline));
    EXPECT_EQ(timer_.total_pending(), WheelTimer::BUCKET_CAPACITY);  // Unchanged
}

// BucketOverflowIncrementsCounter - overflow_dropped stat incremented on overflow
TEST_F(WheelTimerTest, BucketOverflowIncrementsCounter) {
    const std::uint64_t deadline = ms_to_tsc(5);

    // Fill a bucket
    for (std::size_t i = 0; i < WheelTimer::BUCKET_CAPACITY; ++i) {
        ASSERT_TRUE(timer_.schedule(i, 1, deadline));
    }

    EXPECT_EQ(timer_.stats().overflow_dropped, 0u);

    // Try to add more - should increment overflow counter
    EXPECT_FALSE(timer_.schedule(9999, 1, deadline));
    EXPECT_EQ(timer_.stats().overflow_dropped, 1u);

    EXPECT_FALSE(timer_.schedule(9998, 1, deadline));
    EXPECT_EQ(timer_.stats().overflow_dropped, 2u);
}

// PastDeadlineExpiresImmediately - Deadline in past expires on next poll
TEST_F(WheelTimerTest, PastDeadlineExpiresImmediately) {
    // Advance time first
    timer_.advance(ms_to_tsc(100));

    // Schedule a deadline in the past
    ASSERT_TRUE(timer_.schedule(100, 1, ms_to_tsc(50)));  // 50ms is in the past
    EXPECT_EQ(timer_.total_pending(), 1u);

    int callback_count = 0;
    auto on_expired = [&](core::OrderKey, std::uint32_t) {
        ++callback_count;
    };

    // Should expire on next poll
    timer_.poll_expired(ms_to_tsc(101), on_expired);
    EXPECT_EQ(callback_count, 1);
    EXPECT_EQ(timer_.total_pending(), 0u);
}

// FarFutureDeadlineRescheduled - Deadline beyond wheel range is re-scheduled until due
TEST_F(WheelTimerTest, FarFutureDeadlineRescheduled) {
    // Schedule a deadline far beyond the wheel span (256ms)
    const std::uint64_t far_deadline = ms_to_tsc(500);  // 500ms
    ASSERT_TRUE(timer_.schedule(100, 1, far_deadline));

    int callback_count = 0;
    auto on_expired = [&](core::OrderKey, std::uint32_t) {
        ++callback_count;
    };

    // Poll at 260ms - should re-schedule, not expire (deadline is 500ms)
    timer_.poll_expired(ms_to_tsc(260), on_expired);
    EXPECT_EQ(callback_count, 0);
    EXPECT_GE(timer_.stats().rescheduled, 1u);
    EXPECT_EQ(timer_.total_pending(), 1u);  // Still pending

    // Poll at 510ms - should finally expire
    timer_.poll_expired(ms_to_tsc(510), on_expired);
    EXPECT_EQ(callback_count, 1);
    EXPECT_EQ(timer_.total_pending(), 0u);
}

// WheelWrapAround - Wheel correctly wraps from bucket 255 to bucket 0
TEST_F(WheelTimerTest, WheelWrapAround) {
    // Start near the wrap-around point
    const std::uint64_t start_time = ms_to_tsc(250);  // Bucket 250
    WheelTimer wrap_timer{start_time};

    // Schedule deadline that crosses wrap boundary
    const std::uint64_t deadline = ms_to_tsc(260);  // Would be bucket 260, wraps to bucket 4
    ASSERT_TRUE(wrap_timer.schedule(100, 1, deadline));

    int callback_count = 0;
    auto on_expired = [&](core::OrderKey, std::uint32_t) {
        ++callback_count;
    };

    // Poll past wrap point but before deadline
    wrap_timer.poll_expired(ms_to_tsc(258), on_expired);
    EXPECT_EQ(callback_count, 0);

    // Poll after deadline
    wrap_timer.poll_expired(ms_to_tsc(261), on_expired);
    EXPECT_EQ(callback_count, 1);
}

// ResetClearsAllBuckets - reset() empties all buckets and resets stats
TEST_F(WheelTimerTest, ResetClearsAllBuckets) {
    // Schedule entries in multiple buckets
    ASSERT_TRUE(timer_.schedule(100, 1, ms_to_tsc(5)));
    ASSERT_TRUE(timer_.schedule(200, 2, ms_to_tsc(10)));
    ASSERT_TRUE(timer_.schedule(300, 3, ms_to_tsc(50)));
    EXPECT_EQ(timer_.total_pending(), 3u);

    // Force some stats
    int dummy = 0;
    timer_.poll_expired(ms_to_tsc(6), [&](core::OrderKey, std::uint32_t) { ++dummy; });
    EXPECT_GT(timer_.stats().expired, 0u);

    // Reset with new start time
    timer_.reset(ms_to_tsc(1000));

    EXPECT_EQ(timer_.total_pending(), 0u);
    EXPECT_EQ(timer_.stats().scheduled, 0u);
    EXPECT_EQ(timer_.stats().expired, 0u);
    EXPECT_EQ(timer_.stats().rescheduled, 0u);
    EXPECT_EQ(timer_.stats().overflow_dropped, 0u);
    EXPECT_EQ(timer_.current_tick(), 1000u);
    EXPECT_EQ(timer_.last_poll_tsc(), ms_to_tsc(1000));
}

// PollEmptyWheelNoOp - Polling empty wheel doesn't crash, no callbacks
TEST_F(WheelTimerTest, PollEmptyWheelNoOp) {
    int callback_count = 0;
    auto on_expired = [&](core::OrderKey, std::uint32_t) {
        ++callback_count;
    };

    // Poll empty wheel multiple times
    timer_.poll_expired(ms_to_tsc(10), on_expired);
    timer_.poll_expired(ms_to_tsc(100), on_expired);
    timer_.poll_expired(ms_to_tsc(1000), on_expired);

    EXPECT_EQ(callback_count, 0);
    EXPECT_EQ(timer_.stats().expired, 0u);
}

// StatsAccuracy - Verify scheduled, expired, rescheduled, overflow_dropped counts
TEST_F(WheelTimerTest, StatsAccuracy) {
    // Initial stats
    EXPECT_EQ(timer_.stats().scheduled, 0u);
    EXPECT_EQ(timer_.stats().expired, 0u);
    EXPECT_EQ(timer_.stats().rescheduled, 0u);
    EXPECT_EQ(timer_.stats().overflow_dropped, 0u);

    // Schedule 3 entries
    ASSERT_TRUE(timer_.schedule(100, 1, ms_to_tsc(5)));
    ASSERT_TRUE(timer_.schedule(200, 2, ms_to_tsc(10)));
    ASSERT_TRUE(timer_.schedule(300, 3, ms_to_tsc(15)));
    EXPECT_EQ(timer_.stats().scheduled, 3u);

    // Expire 2 entries
    int dummy = 0;
    timer_.poll_expired(ms_to_tsc(11), [&](core::OrderKey, std::uint32_t) { ++dummy; });
    EXPECT_EQ(timer_.stats().expired, 2u);
    EXPECT_EQ(timer_.stats().scheduled, 3u);  // Scheduled count unchanged

    // Test overflow tracking
    const std::uint64_t deadline = ms_to_tsc(50);
    for (std::size_t i = 0; i < WheelTimer::BUCKET_CAPACITY; ++i) {
        (void)timer_.schedule(1000 + i, 1, deadline);
    }
    const auto scheduled_before_overflow = timer_.stats().scheduled;
    
    // This should fail and increment overflow counter
    // Note: stats().scheduled counts attempts (not successes), so it increments
    // even when schedule() returns false due to overflow
    EXPECT_FALSE(timer_.schedule(9999, 1, deadline));
    EXPECT_EQ(timer_.stats().scheduled, scheduled_before_overflow + 1);  // Attempt counted
    EXPECT_EQ(timer_.stats().overflow_dropped, 1u);
}

// Additional test: Verify that constructor initializes properly with non-zero start time
TEST_F(WheelTimerTest, ConstructorWithNonZeroStartTime) {
    const std::uint64_t start = ms_to_tsc(12345);
    WheelTimer timer{start};

    EXPECT_EQ(timer.current_tick(), 12345u);
    EXPECT_EQ(timer.last_poll_tsc(), start);
    EXPECT_EQ(timer.total_pending(), 0u);
}

// Additional test: Verify advance() without processing
TEST_F(WheelTimerTest, AdvanceDoesNotProcessExpirations) {
    ASSERT_TRUE(timer_.schedule(100, 1, ms_to_tsc(5)));

    // Advance past deadline without processing
    timer_.advance(ms_to_tsc(10));

    // Entry should still be pending (not processed)
    EXPECT_EQ(timer_.total_pending(), 1u);
    EXPECT_EQ(timer_.stats().expired, 0u);
}

// Additional test: Multiple entries with same key but different generations
TEST_F(WheelTimerTest, MultipleEntriesSameKeyDifferentGenerations) {
    const core::OrderKey key = 42;
    
    // Schedule same key with different generations
    ASSERT_TRUE(timer_.schedule(key, 1, ms_to_tsc(5)));
    ASSERT_TRUE(timer_.schedule(key, 2, ms_to_tsc(10)));
    ASSERT_TRUE(timer_.schedule(key, 3, ms_to_tsc(15)));
    EXPECT_EQ(timer_.total_pending(), 3u);

    std::vector<std::uint32_t> expired_gens;
    auto on_expired = [&](core::OrderKey k, std::uint32_t g) {
        EXPECT_EQ(k, key);
        expired_gens.push_back(g);
    };

    timer_.poll_expired(ms_to_tsc(20), on_expired);
    
    EXPECT_EQ(expired_gens.size(), 3u);
    // All three generations should have been passed to callback
    EXPECT_NE(std::find(expired_gens.begin(), expired_gens.end(), 1), expired_gens.end());
    EXPECT_NE(std::find(expired_gens.begin(), expired_gens.end(), 2), expired_gens.end());
    EXPECT_NE(std::find(expired_gens.begin(), expired_gens.end(), 3), expired_gens.end());
}

} // namespace
