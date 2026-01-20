#include <gtest/gtest.h>

#include "core/sequence_tracker.hpp"

namespace {

TEST(SequenceTrackerTest, InitTracker) {
    core::SequenceTracker trk{};
    const bool ok = core::init_sequence_tracker(trk, 10);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(trk.initialized);
    EXPECT_EQ(trk.expected_seq, 11);
    EXPECT_EQ(trk.last_seen_seq, 10);
    EXPECT_FALSE(trk.gap_open);
}

TEST(SequenceTrackerTest, InOrderSequence) {
    core::SequenceTracker trk{};
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));
    core::SequenceGapEvent evt{};

    EXPECT_FALSE(core::track_sequence(trk, core::Source::Primary, 0, 2, 0, &evt));
    EXPECT_FALSE(core::track_sequence(trk, core::Source::Primary, 0, 3, 0, &evt));
    EXPECT_FALSE(core::track_sequence(trk, core::Source::Primary, 0, 4, 0, &evt));

    EXPECT_EQ(trk.expected_seq, 5);
    EXPECT_EQ(trk.last_seen_seq, 4);
    EXPECT_FALSE(trk.gap_open);
}

TEST(SequenceTrackerTest, ForwardGapDetected) {
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));
    const bool has_gap = core::track_sequence(trk, core::Source::Primary, 0, 4, 123, &evt);

    EXPECT_TRUE(has_gap);
    EXPECT_TRUE(trk.gap_open);
    EXPECT_EQ(trk.gap_start_seq, 2);
    EXPECT_EQ(evt.kind, core::GapKind::Gap);
    EXPECT_EQ(evt.expected_seq, 2);
    EXPECT_EQ(evt.seen_seq, 4);
    EXPECT_EQ(evt.detect_ts, 123);
}

TEST(SequenceTrackerTest, DuplicateDetected) {
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));
    EXPECT_FALSE(core::track_sequence(trk, core::Source::Primary, 0, 2, 0, &evt));
    const bool dup = core::track_sequence(trk, core::Source::Primary, 0, 2, 55, &evt);

    EXPECT_TRUE(dup);
    EXPECT_EQ(evt.kind, core::GapKind::Duplicate);
    EXPECT_EQ(evt.seen_seq, 2);
    EXPECT_EQ(evt.expected_seq, 3);
}

TEST(SequenceTrackerTest, OutOfOrderDetected) {
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 3, 0, &evt));
    const bool out_of_order = core::track_sequence(trk, core::Source::Primary, 0, 2, 77, &evt);

    EXPECT_TRUE(out_of_order);
    EXPECT_EQ(evt.kind, core::GapKind::OutOfOrder);
    EXPECT_EQ(evt.expected_seq, 4);
    EXPECT_EQ(evt.seen_seq, 2);
}

// ============================================================================
// FX-7054: Enhanced Gap Lifecycle Tests
// ============================================================================

TEST(SequenceTrackerTest, GapDetection_PopulatesLifecycleFields) {
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));

    // Verify initial state
    EXPECT_EQ(trk.gap_opened_tsc, 0u);
    EXPECT_EQ(trk.gap_last_missing_seq, 0u);
    EXPECT_EQ(trk.orders_in_gap_count, 0u);

    // Create gap: expecting seq 2, but receive seq 5
    const std::uint64_t gap_time = 12345;
    const bool has_gap = core::track_sequence(trk, core::Source::Primary, 0, 5, gap_time, &evt);

    EXPECT_TRUE(has_gap);
    EXPECT_TRUE(trk.gap_open);
    EXPECT_EQ(trk.gap_start_seq, 2u);           // First missing
    EXPECT_EQ(trk.gap_last_missing_seq, 4u);    // Last missing (seq 5 - 1)
    EXPECT_EQ(trk.gap_opened_tsc, gap_time);    // Detection time
    EXPECT_EQ(trk.orders_in_gap_count, 0u);     // Reset for new gap
}

TEST(SequenceTrackerTest, GapExtension_UpdatesLastMissing) {
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));

    // Create initial gap: expecting seq 2, but receive seq 4
    const std::uint64_t first_gap_time = 1000;
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 4, first_gap_time, &evt));

    EXPECT_EQ(trk.gap_start_seq, 2u);
    EXPECT_EQ(trk.gap_last_missing_seq, 3u);    // Missing: 2, 3
    EXPECT_EQ(trk.gap_opened_tsc, first_gap_time);
    EXPECT_EQ(trk.gap_epoch, 1u);

    // Extend gap: now expecting seq 5, but receive seq 8 (extending the gap)
    const std::uint64_t extend_time = 2000;
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 8, extend_time, &evt));

    EXPECT_EQ(trk.gap_start_seq, 2u);           // First missing unchanged
    EXPECT_EQ(trk.gap_last_missing_seq, 7u);    // Extended: now missing 2,3,5,6,7
    EXPECT_EQ(trk.gap_opened_tsc, first_gap_time); // Original detection time preserved
}

TEST(SequenceTrackerTest, NewGap_IncrementsEpoch) {
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));

    // Initial state
    EXPECT_EQ(trk.gap_epoch, 0u);

    // First gap
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 4, 1000, &evt));
    EXPECT_EQ(trk.gap_epoch, 1u);

    // Extend gap (should NOT increment epoch)
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 7, 2000, &evt));
    EXPECT_EQ(trk.gap_epoch, 1u);

    // Close gap explicitly
    EXPECT_TRUE(core::close_gap(trk));
    EXPECT_EQ(trk.gap_epoch, 1u);   // Epoch preserved after close

    // Receive some in-order sequences
    EXPECT_FALSE(core::track_sequence(trk, core::Source::Primary, 0, 8, 3000, &evt));
    EXPECT_FALSE(core::track_sequence(trk, core::Source::Primary, 0, 9, 3000, &evt));

    // Second NEW gap - epoch should increment
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 12, 4000, &evt));
    EXPECT_EQ(trk.gap_epoch, 2u);
}

TEST(SequenceTrackerTest, CloseGap_ClearsFields) {
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));

    // Create a gap
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 5, 1000, &evt));

    // Set orders_in_gap_count manually to simulate marked orders
    trk.orders_in_gap_count = 3;

    EXPECT_TRUE(trk.gap_open);
    EXPECT_NE(trk.gap_start_seq, 0u);
    EXPECT_NE(trk.gap_opened_tsc, 0u);
    EXPECT_NE(trk.gap_last_missing_seq, 0u);

    // Close the gap
    const bool was_open = core::close_gap(trk);
    EXPECT_TRUE(was_open);

    // Verify fields are cleared
    EXPECT_FALSE(trk.gap_open);
    EXPECT_EQ(trk.gap_start_seq, 0u);
    EXPECT_EQ(trk.gap_opened_tsc, 0u);
    EXPECT_EQ(trk.gap_last_missing_seq, 0u);
    EXPECT_EQ(trk.orders_in_gap_count, 0u);  // Count reset to prevent corruption

    // Verify epoch preserved for historical tracking
    EXPECT_EQ(trk.gap_epoch, 1u);

    // Verify double-close returns false
    EXPECT_FALSE(core::close_gap(trk));
}

TEST(SequenceTrackerTest, OrdersInGapCount_ResetOnGapClose) {
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));

    // Create gap
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 5, 1000, &evt));

    // Simulate marking some orders
    trk.orders_in_gap_count = 5;

    // Close gap - count should be reset to prevent corruption
    core::close_gap(trk);
    EXPECT_EQ(trk.orders_in_gap_count, 0u);

    // Create new gap - count starts at 0 (no double-reset issue)
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 10, 2000, &evt));
    EXPECT_EQ(trk.orders_in_gap_count, 0u);
}

} // namespace
