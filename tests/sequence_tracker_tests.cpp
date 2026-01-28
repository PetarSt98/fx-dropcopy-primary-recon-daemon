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
    // Create gap: jump to seq 3 (missing seq 2)
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 3, 0, &evt));
    // Receive seq 2 out-of-order - this fills the gap [2, 3)
    const bool out_of_order = core::track_sequence(trk, core::Source::Primary, 0, 2, 77, &evt);

    EXPECT_TRUE(out_of_order);
    // Since seq 2 falls in gap range [2, 3), it closes the gap → GapFill
    EXPECT_EQ(evt.kind, core::GapKind::GapFill);
    EXPECT_TRUE(evt.gap_closed_by_fill);
    EXPECT_EQ(evt.expected_seq, 4);
    EXPECT_EQ(evt.seen_seq, 2);
}

TEST(SequenceTrackerTest, GapClosedByFill) {
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    
    // Initialize with seq 1
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));
    
    // Create a gap: receive seq 5 (gap: 2, 3, 4)
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 5, 100, &evt));
    EXPECT_TRUE(trk.gap_open);
    EXPECT_EQ(trk.gap_start_seq, 2);
    EXPECT_EQ(trk.gap_end_seq, 5);
    EXPECT_FALSE(evt.gap_closed_by_fill);  // Gap just opened, not closed
    
    // Now receive seq 3 (out-of-order, fills part of gap)
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 3, 200, &evt));
    // When gap is closed by fill, kind should be GapFill (not OutOfOrder)
    EXPECT_EQ(evt.kind, core::GapKind::GapFill);
    EXPECT_TRUE(evt.gap_closed_by_fill);  // Gap should be closed by this fill
    EXPECT_FALSE(trk.gap_open);  // Gap should be closed now
}

TEST(SequenceTrackerTest, GapNotClosedByDuplicate) {
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    
    // Initialize with seq 1
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));
    EXPECT_FALSE(core::track_sequence(trk, core::Source::Primary, 0, 2, 0, &evt));
    
    // Create a gap: receive seq 5 (gap: 3, 4)
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 5, 100, &evt));
    EXPECT_TRUE(trk.gap_open);
    // After seq 5: last_seen_seq=5, gap_start_seq=3, gap_end_seq=5
    
    // Receive duplicate of seq 5 (last_seen_seq) - should not close gap
    // Note: Duplicate means seq == last_seen_seq, so we send 5 again
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 5, 200, &evt));
    EXPECT_EQ(evt.kind, core::GapKind::Duplicate);
    EXPECT_FALSE(evt.gap_closed_by_fill);  // Duplicate should not close gap
    EXPECT_TRUE(trk.gap_open);  // Gap should still be open
}

TEST(SequenceTrackerTest, OutOfOrderOutsideGapRange) {
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    
    // Initialize with seq 10
    ASSERT_TRUE(core::init_sequence_tracker(trk, 10));
    EXPECT_FALSE(core::track_sequence(trk, core::Source::Primary, 0, 11, 0, &evt));
    
    // Create a gap: receive seq 15 (gap: 12, 13, 14)
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 15, 100, &evt));
    EXPECT_TRUE(trk.gap_open);
    EXPECT_EQ(trk.gap_start_seq, 12);
    EXPECT_EQ(trk.gap_end_seq, 15);
    
    // Receive seq 10 (out-of-order but OUTSIDE gap range [12, 15))
    // seq 10 < gap_start_seq 12, so it's just OutOfOrder, not GapFill
    // Note: last_seen_seq is now 15, so seq 10 is out-of-order relative to expected_seq 16
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 10, 200, &evt));
    EXPECT_EQ(evt.kind, core::GapKind::OutOfOrder);  // Outside gap range → OutOfOrder
    EXPECT_FALSE(evt.gap_closed_by_fill);
    EXPECT_TRUE(trk.gap_open);  // Gap should still be open
}

// Test that gap_epoch increments past 65535 without wrapping (uint32_t fix)
TEST(SequenceTrackerTest, GapEpochDoesNotWrapAt65535) {
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    
    // Initialize tracker
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));
    
    // Simulate 65,536 gaps (old uint16_t would wrap at 65535→0)
    // Start epoch at 65534 to test the boundary
    trk.gap_epoch = 65534;
    
    // Create first gap - epoch should go to 65535
    std::uint64_t seq = 10;
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, seq, 100, &evt));
    EXPECT_EQ(trk.gap_epoch, 65535u);
    
    // Close gap and create another - epoch should go to 65536 (not wrap to 0)
    core::close_gap(trk);
    trk.expected_seq = seq + 1;
    seq = 20;
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, seq, 200, &evt));
    EXPECT_EQ(trk.gap_epoch, 65536u);  // Should be 65536, not 0
    
    // Continue past the old boundary
    core::close_gap(trk);
    trk.expected_seq = seq + 1;
    seq = 30;
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, seq, 300, &evt));
    EXPECT_EQ(trk.gap_epoch, 65537u);
}

// Test that gap_epoch skips 0 (sentinel value) on wrap-around
TEST(SequenceTrackerTest, GapEpochSkipsZeroOnWrapAround) {
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    
    // Initialize tracker
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));
    
    // Set epoch to max uint32_t - 1 to test wrap-around
    trk.gap_epoch = 0xFFFFFFFE;  // 4294967294
    
    // First gap: epoch should go to 0xFFFFFFFF
    std::uint64_t seq = 10;
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, seq, 100, &evt));
    EXPECT_EQ(trk.gap_epoch, 0xFFFFFFFFu);
    
    // Close gap and create another - epoch would wrap to 0, but should skip to 1
    core::close_gap(trk);
    trk.expected_seq = seq + 1;
    seq = 20;
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, seq, 200, &evt));
    EXPECT_EQ(trk.gap_epoch, 1u);  // Should be 1, not 0 (0 is sentinel)
}

// Test that gap_epoch starts at 1 for first gap (not 0)
TEST(SequenceTrackerTest, GapEpochStartsAtOneNotZero) {
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    
    // Initialize tracker
    ASSERT_TRUE(core::init_sequence_tracker(trk, 1));
    EXPECT_EQ(trk.gap_epoch, 0u);  // No gaps yet, so 0 (sentinel)
    
    // Create first gap - epoch should be 1
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 5, 100, &evt));
    EXPECT_EQ(trk.gap_epoch, 1u);  // First gap, epoch should be 1
    
    // Close and create another gap - epoch should be 2
    core::close_gap(trk);
    trk.expected_seq = 6;
    EXPECT_TRUE(core::track_sequence(trk, core::Source::Primary, 0, 10, 200, &evt));
    EXPECT_EQ(trk.gap_epoch, 2u);
}

} // namespace
