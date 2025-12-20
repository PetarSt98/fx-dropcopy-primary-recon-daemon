#include "test_main.hpp"

#include <atomic>
#include <cstring>
#include <memory>

#include "core/reconciler.hpp"
#include "core/order_state_store.hpp"
#include "ingest/spsc_ring.hpp"
#include "util/arena.hpp"

namespace reconciler_sequence_tests {

using ExecRing = ingest::SpscRing<core::ExecEvent, 1u << 16>;

core::ExecEvent make_seq_event(core::Source src, std::uint64_t seq, const char* clord) {
    core::ExecEvent ev{};
    ev.source = src;
    ev.seq_num = seq;
    ev.ord_status = core::OrdStatus::New;
    ev.exec_type = core::ExecType::New;
    ev.cum_qty = 0;
    ev.qty = 0;
    ev.price_micro = 0;
    ev.ingest_tsc = seq;
    ev.set_clord_id(clord, std::strlen(clord));
    ev.set_exec_id("X", 1);
    return ev;
}

struct Harness {
    std::atomic<bool> stop_flag{false};
    std::unique_ptr<ExecRing> primary_ring;
    std::unique_ptr<ExecRing> dropcopy_ring;
    std::unique_ptr<core::DivergenceRing> divergence_ring;
    std::unique_ptr<core::SequenceGapRing> seq_gap_ring;
    util::Arena arena{util::Arena::default_capacity_bytes};
    core::OrderStateStore store;
    core::ReconCounters counters{};
    core::Reconciler reconciler;

    Harness()
        : primary_ring(std::make_unique<ExecRing>()),
          dropcopy_ring(std::make_unique<ExecRing>()),
          divergence_ring(std::make_unique<core::DivergenceRing>()),
          seq_gap_ring(std::make_unique<core::SequenceGapRing>()),
          store(arena, 128u),
          reconciler(stop_flag,
                     *primary_ring,
                     *dropcopy_ring,
                     store,
                     counters,
                     *divergence_ring,
                     *seq_gap_ring) {}
};

bool test_primary_gap_emitted() {
    Harness h;
    auto ev1 = make_seq_event(core::Source::Primary, 1, "CP1");
    auto ev2 = make_seq_event(core::Source::Primary, 4, "CP1");
    h.reconciler.process_event_for_test(ev1);
    h.reconciler.process_event_for_test(ev2);

    core::SequenceGapEvent gap{};
    if (!h.seq_gap_ring->try_pop(gap)) return false;
    if (gap.kind != core::GapKind::Gap) return false;
    return h.counters.primary_seq_gaps == 1 && h.counters.sequence_gap_ring_drops == 0 &&
           gap.expected_seq == 2 && gap.seen_seq == 4;
}

bool test_dropcopy_duplicate() {
    Harness h;
    auto ev1 = make_seq_event(core::Source::DropCopy, 1, "CD1");
    auto ev2 = make_seq_event(core::Source::DropCopy, 2, "CD1");
    auto ev3 = make_seq_event(core::Source::DropCopy, 2, "CD1");
    h.reconciler.process_event_for_test(ev1);
    h.reconciler.process_event_for_test(ev2);
    h.reconciler.process_event_for_test(ev3);

    core::SequenceGapEvent gap{};
    // Pop both events if present; we expect one duplicate entry
    bool saw_duplicate = false;
    while (h.seq_gap_ring->try_pop(gap)) {
        if (gap.kind == core::GapKind::Duplicate) {
            saw_duplicate = true;
        }
    }
    return saw_duplicate && h.counters.dropcopy_seq_duplicates == 1 && h.counters.sequence_gap_ring_drops == 0;
}

void add_tests(std::vector<TestCase>& tests) {
    tests.push_back({"reconciler_primary_gap_emitted", test_primary_gap_emitted});
    tests.push_back({"reconciler_dropcopy_duplicate", test_dropcopy_duplicate});
}

} // namespace reconciler_sequence_tests
