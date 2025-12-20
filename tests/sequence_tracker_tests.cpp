#include "test_main.hpp"

#include "core/sequence_tracker.hpp"

namespace sequence_tracker_tests {

bool test_init_tracker() {
    core::SequenceTracker trk{};
    const bool ok = core::init_sequence_tracker(trk, 10);
    return ok && trk.initialized && trk.expected_seq == 11 && trk.last_seen_seq == 10 && !trk.gap_open;
}

bool test_in_order_sequence() {
    core::SequenceTracker trk{};
    core::init_sequence_tracker(trk, 1);
    core::SequenceGapEvent evt{};
    if (core::track_sequence(trk, core::Source::Primary, 0, 2, 0, &evt)) return false;
    if (core::track_sequence(trk, core::Source::Primary, 0, 3, 0, &evt)) return false;
    if (core::track_sequence(trk, core::Source::Primary, 0, 4, 0, &evt)) return false;
    return trk.expected_seq == 5 && trk.last_seen_seq == 4 && !trk.gap_open;
}

bool test_forward_gap_detected() {
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    core::init_sequence_tracker(trk, 1);
    const bool has_gap = core::track_sequence(trk, core::Source::Primary, 0, 4, 123, &evt);
    return has_gap &&
           trk.gap_open &&
           trk.gap_start_seq == 2 &&
           evt.kind == core::GapKind::Gap &&
           evt.expected_seq == 2 &&
           evt.seen_seq == 4 &&
           evt.detect_ts == 123;
}

bool test_duplicate_detected() {
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    core::init_sequence_tracker(trk, 1);
    core::track_sequence(trk, core::Source::Primary, 0, 2, 0, &evt);
    const bool dup = core::track_sequence(trk, core::Source::Primary, 0, 2, 55, &evt);
    return dup && evt.kind == core::GapKind::Duplicate && evt.seen_seq == 2 && evt.expected_seq == 3;
}

bool test_out_of_order_detected() {
    core::SequenceTracker trk{};
    core::SequenceGapEvent evt{};
    core::init_sequence_tracker(trk, 1);
    core::track_sequence(trk, core::Source::Primary, 0, 3, 0, &evt); // opens a gap at 2
    const bool out_of_order = core::track_sequence(trk, core::Source::Primary, 0, 2, 77, &evt);
    return out_of_order && evt.kind == core::GapKind::OutOfOrder && evt.expected_seq == 4 && evt.seen_seq == 2;
}

void add_tests(std::vector<TestCase>& tests) {
    tests.push_back({"sequence_tracker_init", test_init_tracker});
    tests.push_back({"sequence_tracker_in_order", test_in_order_sequence});
    tests.push_back({"sequence_tracker_forward_gap", test_forward_gap_detected});
    tests.push_back({"sequence_tracker_duplicate", test_duplicate_detected});
    tests.push_back({"sequence_tracker_out_of_order", test_out_of_order_detected});
}

} // namespace sequence_tracker_tests
