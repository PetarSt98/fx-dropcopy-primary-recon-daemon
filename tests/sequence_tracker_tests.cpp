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
// FX-7054: Gap Lifecycle Tracking Tests
// ============================================================================

TEST(SequenceTrackerTest, GapLifecycleFieldsPopulated) {
    // Verify gap_opened_tsc, gap_first_missing_seq, gap_last_missing_seq are set correctly
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));
    
    // Expected next: 2. Send seq 5 -> gap of 2,3,4
    const bool has_gap = core::track_sequence(trk, core::Source::Primary, 0, 5, 1000, &evt);
    
    EXPECT_TRUE(has_gap);
    EXPECT_TRUE(trk.gap_open);
    EXPECT_EQ(trk.gap_opened_tsc, 1000u);           // TSC when gap detected
    EXPECT_EQ(trk.gap_first_missing_seq, 2u);       // First missing is 2
    EXPECT_EQ(trk.gap_last_missing_seq, 4u);        // Last missing is 5-1=4
    EXPECT_EQ(trk.gap_start_seq, 2u);               // Existing field
    EXPECT_EQ(trk.gap_epoch, 1u);                   // First gap
}

TEST(SequenceTrackerTest, GapClosureDetected) {
    // Verify gap_open becomes false when sequences fill in
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));
    
    // Create gap: expecting 2, receive 4 -> missing 2,3
    core::track_sequence(trk, core::Source::Primary, 0, 4, 1000, &evt);
    EXPECT_TRUE(trk.gap_open);
    EXPECT_EQ(trk.gap_first_missing_seq, 2u);
    EXPECT_EQ(trk.gap_last_missing_seq, 3u);
    EXPECT_EQ(trk.expected_seq, 5u);
    
    // Fill in sequence 5 (in order)
    // After this, expected_seq = 6, which is > gap_last_missing_seq (3)
    // Gap should close since expected_seq (6) > gap_last_missing_seq (3)
    core::track_sequence(trk, core::Source::Primary, 0, 5, 1001, &evt);
    EXPECT_FALSE(trk.gap_open);  // Gap should have closed
    EXPECT_EQ(trk.gap_start_seq, 0u);
    EXPECT_EQ(trk.gap_opened_tsc, 0u);
    EXPECT_EQ(trk.gap_first_missing_seq, 0u);
    EXPECT_EQ(trk.gap_last_missing_seq, 0u);
    // gap_epoch should be preserved
    EXPECT_EQ(trk.gap_epoch, 1u);
}

TEST(SequenceTrackerTest, GapEpochIncrementsOnNewGap) {
    // Verify gap_epoch increases only on NEW gaps, not extends
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));
    
    EXPECT_EQ(trk.gap_epoch, 0u);
    
    // First gap: expecting 2, receive 4
    core::track_sequence(trk, core::Source::Primary, 0, 4, 1000, &evt);
    EXPECT_EQ(trk.gap_epoch, 1u);
    EXPECT_TRUE(trk.gap_open);
    EXPECT_EQ(trk.gap_last_missing_seq, 3u);
    
    // Extend gap: gap still open, receive 7 -> extends gap
    core::track_sequence(trk, core::Source::Primary, 0, 7, 1001, &evt);
    EXPECT_EQ(trk.gap_epoch, 1u);  // Should NOT increment (gap extend, not new gap)
    EXPECT_TRUE(trk.gap_open);
    EXPECT_EQ(trk.gap_last_missing_seq, 6u);  // Extended to include 5,6
    
    // Close the gap by receiving in-order sequence that advances past gap_last_missing_seq
    core::track_sequence(trk, core::Source::Primary, 0, 8, 1002, &evt);
    EXPECT_FALSE(trk.gap_open);  // Gap closed
    EXPECT_EQ(trk.gap_epoch, 1u);  // Epoch preserved after closure
    
    // Create NEW gap: expecting 9, receive 11
    core::track_sequence(trk, core::Source::Primary, 0, 11, 2000, &evt);
    EXPECT_EQ(trk.gap_epoch, 2u);  // Should increment (new gap)
    EXPECT_TRUE(trk.gap_open);
    EXPECT_EQ(trk.gap_first_missing_seq, 9u);
    EXPECT_EQ(trk.gap_last_missing_seq, 10u);
}

TEST(SequenceTrackerTest, OrdersInGapUncertaintyTracked) {
    // Verify orders_in_gap_uncertainty counter (will be used in Part 2)
    core::SequenceTracker trk{};
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));
    
    // Initial value should be 0
    EXPECT_EQ(trk.orders_in_gap_uncertainty, 0u);
    
    // Manually set to verify it's preserved (actual increment logic is in Part 2)
    trk.orders_in_gap_uncertainty = 5;
    EXPECT_EQ(trk.orders_in_gap_uncertainty, 5u);
    
    // Create and close a gap - counter should be preserved
    core::SequenceGapEvent evt{};
    core::track_sequence(trk, core::Source::Primary, 0, 4, 1000, &evt);  // Create gap
    EXPECT_TRUE(trk.gap_open);
    
    core::track_sequence(trk, core::Source::Primary, 0, 5, 1001, &evt);  // Close gap
    EXPECT_FALSE(trk.gap_open);
    
    // orders_in_gap_uncertainty should be preserved after gap closure
    EXPECT_EQ(trk.orders_in_gap_uncertainty, 5u);
}

} // namespace
