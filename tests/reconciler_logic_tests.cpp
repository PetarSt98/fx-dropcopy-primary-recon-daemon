#include "test_main.hpp"

#include <cstring>
#include <memory>

#include "core/reconciler.hpp"
#include "core/divergence.hpp"
#include "core/order_state_store.hpp"
#include "ingest/spsc_ring.hpp"
#include "util/arena.hpp"

namespace reconciler_logic_tests {

using ExecRing = ingest::SpscRing<core::ExecEvent, 1u << 16>;

core::ExecEvent make_event(core::Source src,
                           core::OrdStatus status,
                           std::int64_t cum_qty,
                           std::int64_t price_micro,
                           std::uint64_t ts,
                           const char* clord_id = "CID1",
                           const char* exec_id = "EXEC1") {
    core::ExecEvent ev{};
    ev.source = src;
    ev.ord_status = status;
    switch (status) {
    case core::OrdStatus::Filled:
        ev.exec_type = core::ExecType::Fill;
        break;
    case core::OrdStatus::PartiallyFilled:
        ev.exec_type = core::ExecType::PartialFill;
        break;
    default:
        ev.exec_type = core::ExecType::New;
        break;
    }
    ev.cum_qty = cum_qty;
    ev.qty = cum_qty;
    ev.price_micro = price_micro;
    ev.transact_time = ts;
    ev.set_clord_id(clord_id, std::strlen(clord_id));
    ev.set_exec_id(exec_id, std::strlen(exec_id));
    return ev;
}

struct Harness {
    std::atomic<bool> stop_flag{false};
    std::unique_ptr<ExecRing> primary_ring;
    std::unique_ptr<ExecRing> dropcopy_ring;
    std::unique_ptr<core::DivergenceRing> divergence_ring;
    util::Arena arena{util::Arena::default_capacity_bytes};
    core::OrderStateStore store;
    core::ReconCounters counters{};
    core::Reconciler reconciler;

    explicit Harness(std::size_t capacity_hint = 128u)
        : primary_ring(std::make_unique<ExecRing>()),
          dropcopy_ring(std::make_unique<ExecRing>()),
          divergence_ring(std::make_unique<core::DivergenceRing>()),
          store(arena, capacity_hint),
          reconciler(stop_flag, *primary_ring, *dropcopy_ring, store, counters, *divergence_ring) {}
};

bool test_matching_views_no_divergence() {
    Harness h;
    auto internal = make_event(core::Source::Primary, core::OrdStatus::Working, 10, 100, 10, "CIDM", "INT1");
    auto dropcopy = make_event(core::Source::DropCopy, core::OrdStatus::Working, 10, 100, 12, "CIDM", "DC1");

    h.reconciler.process_event_for_test(internal);
    h.reconciler.process_event_for_test(dropcopy);

    core::Divergence div{};
    if (h.divergence_ring->try_pop(div)) return false;
    return h.counters.divergence_total == 0 && h.counters.internal_events == 1 && h.counters.dropcopy_events == 1 &&
           h.counters.divergence_ring_drops == 0;
}

bool test_missing_fill_emitted() {
    Harness h;
    auto internal = make_event(core::Source::Primary, core::OrdStatus::Working, 10, 100, 10, "CID1", "INT1");
    auto dropcopy = make_event(core::Source::DropCopy, core::OrdStatus::Filled, 20, 105, 12, "CID1", "DC1");

    h.reconciler.process_event_for_test(internal);
    h.reconciler.process_event_for_test(dropcopy);

    core::Divergence div{};
    if (!h.divergence_ring->try_pop(div)) return false;
    if (div.type != core::DivergenceType::MissingFill) return false;
    return h.counters.divergence_total == 1 &&
           h.counters.divergence_missing_fill == 1 &&
           h.counters.divergence_ring_drops == 0;
}

bool test_phantom_order_emitted() {
    Harness h;
    auto dropcopy = make_event(core::Source::DropCopy, core::OrdStatus::New, 0, 100, 10, "CID2", "DC2");
    h.reconciler.process_event_for_test(dropcopy);

    core::Divergence div{};
    if (!h.divergence_ring->try_pop(div)) return false;
    return div.type == core::DivergenceType::PhantomOrder &&
           h.counters.divergence_total == 1 &&
           h.counters.divergence_phantom == 1;
}

bool test_state_and_quantity_mismatches() {
    Harness h;
    auto internal_state = make_event(core::Source::Primary, core::OrdStatus::PartiallyFilled, 50, 100, 10, "CID3", "INT3");
    auto dropcopy_state = make_event(core::Source::DropCopy, core::OrdStatus::Filled, 50, 100, 12, "CID3", "DC3");

    h.reconciler.process_event_for_test(internal_state);
    h.reconciler.process_event_for_test(dropcopy_state);

    core::Divergence first{};
    if (!h.divergence_ring->try_pop(first)) return false;
    if (first.type != core::DivergenceType::StateMismatch) return false;
    if (h.counters.divergence_state_mismatch != 1) return false;

    // Reset for a quantity mismatch scenario on a different order id.
    auto internal_qty = make_event(core::Source::Primary, core::OrdStatus::Filled, 10, 100, 20, "CID4", "INT4");
    auto dropcopy_qty = make_event(core::Source::DropCopy, core::OrdStatus::Filled, 20, 100, 22, "CID4", "DC4");

    h.reconciler.process_event_for_test(internal_qty);
    h.reconciler.process_event_for_test(dropcopy_qty);

    core::Divergence second{};
    if (!h.divergence_ring->try_pop(second)) return false;
    if (second.type != core::DivergenceType::QuantityMismatch) return false;
    return h.counters.divergence_quantity_mismatch == 1 &&
           h.counters.divergence_total == 2;
}

bool test_integration_style_ring_draining() {
    Harness h;
    auto p1 = make_event(core::Source::Primary, core::OrdStatus::New, 0, 100, 10, "CID5", "P1");
    auto p2 = make_event(core::Source::Primary, core::OrdStatus::Working, 0, 100, 11, "CID5", "P2");
    auto p3 = make_event(core::Source::Primary, core::OrdStatus::PartiallyFilled, 50, 100, 12, "CID5", "P3");
    auto d1 = make_event(core::Source::DropCopy, core::OrdStatus::PartiallyFilled, 50, 100, 13, "CID5", "D1");
    auto d2 = make_event(core::Source::DropCopy, core::OrdStatus::Filled, 100, 100, 14, "CID5", "D2");

    h.primary_ring->try_push(p1);
    h.primary_ring->try_push(p2);
    h.primary_ring->try_push(p3);
    h.dropcopy_ring->try_push(d1);
    h.dropcopy_ring->try_push(d2);

    core::ExecEvent ev{};
    core::ExecEvent dc{};
    while (true) {
        const bool cp = h.primary_ring->try_pop(ev);
        const bool cd = h.dropcopy_ring->try_pop(dc);
        if (!cp && !cd) break;
        if (cp) h.reconciler.process_event_for_test(ev);
        if (cd) h.reconciler.process_event_for_test(dc);
    }

    core::Divergence div{};
    bool saw_missing_fill = false;
    while (h.divergence_ring->try_pop(div)) {
        if (div.type == core::DivergenceType::MissingFill) {
            saw_missing_fill = true;
        }
    }

    return saw_missing_fill &&
           h.counters.internal_events == 3 &&
           h.counters.dropcopy_events == 2 &&
           h.counters.divergence_total == 1 &&
           h.counters.divergence_ring_drops == 0;
}

void add_tests(std::vector<TestCase>& tests) {
    tests.push_back({"reconciler_matching_views_no_divergence", test_matching_views_no_divergence});
    tests.push_back({"reconciler_missing_fill_emitted", test_missing_fill_emitted});
    tests.push_back({"reconciler_phantom_order_emitted", test_phantom_order_emitted});
    tests.push_back({"reconciler_state_and_quantity_mismatches", test_state_and_quantity_mismatches});
    tests.push_back({"reconciler_integration_style_ring_draining", test_integration_style_ring_draining});
}

} // namespace reconciler_logic_tests
