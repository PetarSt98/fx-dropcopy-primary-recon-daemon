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

} // namespace
