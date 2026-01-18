#include <gtest/gtest.h>

#include "core/recon_timer.hpp"

namespace {

using WheelTimer = util::WheelTimer;

class ReconTimerTest : public ::testing::Test {
protected:
    // Start at time 0 for simplicity
    WheelTimer timer_{0};

    // Helper constants
    static constexpr std::uint64_t MS = WheelTimer::TICK_NS;  // 1ms in ns

    // Create a minimal OrderState for testing
    core::OrderState make_order_state(core::OrderKey key) {
        core::OrderState os{};
        os.key = key;
        os.timer_generation = 0;
        os.recon_deadline_tsc = 0;
        return os;
    }
};

// ReconTimer_Schedule_IncrementsGeneration - timer_generation increases on schedule
TEST_F(ReconTimerTest, Schedule_IncrementsGeneration) {
    core::OrderState os = make_order_state(12345);
    const std::uint32_t initial_gen = os.timer_generation;

    (void)core::schedule_recon_deadline(timer_, os, 5 * MS);

    EXPECT_EQ(os.timer_generation, initial_gen + 1);

    // Schedule again - should increment again
    (void)core::schedule_recon_deadline(timer_, os, 10 * MS);
    EXPECT_EQ(os.timer_generation, initial_gen + 2);
}

// ReconTimer_Schedule_SetsDeadlineTsc - recon_deadline_tsc is set correctly
TEST_F(ReconTimerTest, Schedule_SetsDeadlineTsc) {
    core::OrderState os = make_order_state(12345);
    const std::uint64_t deadline = 50 * MS;

    (void)core::schedule_recon_deadline(timer_, os, deadline);

    EXPECT_EQ(os.recon_deadline_tsc, deadline);

    // Schedule with new deadline - should update
    const std::uint64_t new_deadline = 100 * MS;
    (void)core::schedule_recon_deadline(timer_, os, new_deadline);
    EXPECT_EQ(os.recon_deadline_tsc, new_deadline);
}

// ReconTimer_Schedule_ReturnsWheelResult - propagates wheel's success/failure
TEST_F(ReconTimerTest, Schedule_ReturnsWheelResult) {
    core::OrderState os = make_order_state(12345);
    const std::uint64_t deadline = 5 * MS;

    // First schedule should succeed
    EXPECT_TRUE(core::schedule_recon_deadline(timer_, os, deadline));

    // Fill the bucket to capacity
    for (std::size_t i = 0; i < WheelTimer::BUCKET_CAPACITY - 1; ++i) {
        core::OrderState tmp = make_order_state(1000 + i);
        ASSERT_TRUE(core::schedule_recon_deadline(timer_, tmp, deadline));
    }

    // Bucket is now full - next schedule should fail
    core::OrderState overflow_os = make_order_state(99999);
    EXPECT_FALSE(core::schedule_recon_deadline(timer_, overflow_os, deadline));
}

// ReconTimer_Cancel_IncrementsGeneration - timer_generation increases on cancel
TEST_F(ReconTimerTest, Cancel_IncrementsGeneration) {
    core::OrderState os = make_order_state(12345);

    // Schedule first
    (void)core::schedule_recon_deadline(timer_, os, 5 * MS);
    const std::uint32_t gen_after_schedule = os.timer_generation;

    // Cancel - should increment generation
    core::cancel_recon_deadline(os);
    EXPECT_EQ(os.timer_generation, gen_after_schedule + 1);

    // Cancel again - should increment again
    core::cancel_recon_deadline(os);
    EXPECT_EQ(os.timer_generation, gen_after_schedule + 2);
}

// ReconTimer_Cancel_ClearsDeadlineTsc - recon_deadline_tsc is set to 0
TEST_F(ReconTimerTest, Cancel_ClearsDeadlineTsc) {
    core::OrderState os = make_order_state(12345);
    const std::uint64_t deadline = 50 * MS;

    // Schedule first
    (void)core::schedule_recon_deadline(timer_, os, deadline);
    EXPECT_EQ(os.recon_deadline_tsc, deadline);

    // Cancel - should clear deadline
    core::cancel_recon_deadline(os);
    EXPECT_EQ(os.recon_deadline_tsc, 0u);
}

// ReconTimer_IsValid_MatchingGeneration - returns true when generations match
TEST_F(ReconTimerTest, IsValid_MatchingGeneration) {
    core::OrderState os = make_order_state(12345);

    // Schedule and capture generation
    (void)core::schedule_recon_deadline(timer_, os, 5 * MS);
    const std::uint32_t scheduled_gen = os.timer_generation;

    // Should be valid immediately after schedule
    EXPECT_TRUE(core::is_timer_valid(os, scheduled_gen));
}

// ReconTimer_IsValid_StaleGeneration - returns false when generation mismatch
TEST_F(ReconTimerTest, IsValid_StaleGeneration) {
    core::OrderState os = make_order_state(12345);

    // Schedule and capture generation
    (void)core::schedule_recon_deadline(timer_, os, 5 * MS);
    const std::uint32_t scheduled_gen = os.timer_generation;

    // Cancel - increments generation
    core::cancel_recon_deadline(os);

    // Old generation should now be invalid
    EXPECT_FALSE(core::is_timer_valid(os, scheduled_gen));

    // Current generation should be valid
    EXPECT_TRUE(core::is_timer_valid(os, os.timer_generation));
}

// ReconTimer_Refresh_InvalidatesOldTimer - old generation becomes invalid
TEST_F(ReconTimerTest, Refresh_InvalidatesOldTimer) {
    core::OrderState os = make_order_state(12345);

    // Schedule initial timer
    (void)core::schedule_recon_deadline(timer_, os, 5 * MS);
    const std::uint32_t old_gen = os.timer_generation;

    // Refresh with new deadline
    (void)core::refresh_recon_deadline(timer_, os, 10 * MS);
    const std::uint32_t new_gen = os.timer_generation;

    // Old generation should be invalid
    EXPECT_FALSE(core::is_timer_valid(os, old_gen));

    // New generation should be valid
    EXPECT_TRUE(core::is_timer_valid(os, new_gen));

    // Generations should be different
    EXPECT_NE(old_gen, new_gen);
}

// ReconTimer_Refresh_SetsNewDeadline - new deadline is stored
TEST_F(ReconTimerTest, Refresh_SetsNewDeadline) {
    core::OrderState os = make_order_state(12345);

    // Schedule initial timer
    const std::uint64_t old_deadline = 50 * MS;
    (void)core::schedule_recon_deadline(timer_, os, old_deadline);
    EXPECT_EQ(os.recon_deadline_tsc, old_deadline);

    // Refresh with new deadline
    const std::uint64_t new_deadline = 100 * MS;
    EXPECT_TRUE(core::refresh_recon_deadline(timer_, os, new_deadline));
    EXPECT_EQ(os.recon_deadline_tsc, new_deadline);
}

// ReconTimer_FullCycle_ScheduleCancelReschedule - integration test of full pattern
TEST_F(ReconTimerTest, FullCycle_ScheduleCancelReschedule) {
    core::OrderState os = make_order_state(12345);

    // Initial state
    EXPECT_EQ(os.timer_generation, 0u);
    EXPECT_EQ(os.recon_deadline_tsc, 0u);

    // Step 1: Schedule a timer
    const std::uint64_t deadline1 = 10 * MS;
    EXPECT_TRUE(core::schedule_recon_deadline(timer_, os, deadline1));
    EXPECT_EQ(os.timer_generation, 1u);
    EXPECT_EQ(os.recon_deadline_tsc, deadline1);
    const std::uint32_t gen1 = os.timer_generation;

    // Verify timer is valid
    EXPECT_TRUE(core::is_timer_valid(os, gen1));

    // Step 2: Cancel the timer
    core::cancel_recon_deadline(os);
    EXPECT_EQ(os.timer_generation, 2u);
    EXPECT_EQ(os.recon_deadline_tsc, 0u);

    // Old timer should now be invalid
    EXPECT_FALSE(core::is_timer_valid(os, gen1));

    // Step 3: Reschedule a new timer
    const std::uint64_t deadline2 = 20 * MS;
    EXPECT_TRUE(core::schedule_recon_deadline(timer_, os, deadline2));
    EXPECT_EQ(os.timer_generation, 3u);
    EXPECT_EQ(os.recon_deadline_tsc, deadline2);
    const std::uint32_t gen2 = os.timer_generation;

    // New timer is valid, old ones are not
    EXPECT_TRUE(core::is_timer_valid(os, gen2));
    EXPECT_FALSE(core::is_timer_valid(os, gen1));

    // Step 4: Refresh the timer
    const std::uint64_t deadline3 = 30 * MS;
    EXPECT_TRUE(core::refresh_recon_deadline(timer_, os, deadline3));
    EXPECT_EQ(os.timer_generation, 4u);
    EXPECT_EQ(os.recon_deadline_tsc, deadline3);
    const std::uint32_t gen3 = os.timer_generation;

    // Only latest timer is valid
    EXPECT_TRUE(core::is_timer_valid(os, gen3));
    EXPECT_FALSE(core::is_timer_valid(os, gen2));
    EXPECT_FALSE(core::is_timer_valid(os, gen1));

    // Step 5: Verify the wheel integration - poll and check callbacks
    std::vector<std::pair<core::OrderKey, std::uint32_t>> expired_entries;
    auto on_expired = [&](core::OrderKey key, std::uint32_t gen) {
        expired_entries.emplace_back(key, gen);
    };

    // Poll past all deadlines - multiple entries should expire
    timer_.poll_expired(35 * MS, on_expired);

    // Should have received callback(s) for this order
    // The wheel may have multiple entries (from schedule/refresh)
    // but only the latest generation should be valid
    bool found_valid = false;
    for (const auto& [key, gen] : expired_entries) {
        if (key == os.key) {
            if (core::is_timer_valid(os, gen)) {
                // This is the valid timer
                EXPECT_EQ(gen, gen3);
                found_valid = true;
            }
            // Other entries with old generations are stale - should be skipped by caller
        }
    }
    EXPECT_TRUE(found_valid) << "Expected to find valid timer callback";
}

// Additional test: Verify noexcept guarantees
TEST_F(ReconTimerTest, FunctionsAreNoexcept) {
    // These should compile and be noexcept
    core::OrderState os = make_order_state(12345);

    static_assert(noexcept(core::schedule_recon_deadline(timer_, os, 5 * MS)),
                  "schedule_recon_deadline must be noexcept");
    static_assert(noexcept(core::cancel_recon_deadline(os)),
                  "cancel_recon_deadline must be noexcept");
    static_assert(noexcept(core::refresh_recon_deadline(timer_, os, 10 * MS)),
                  "refresh_recon_deadline must be noexcept");
    static_assert(noexcept(core::is_timer_valid(os, 0)),
                  "is_timer_valid must be noexcept");
}

} // namespace
