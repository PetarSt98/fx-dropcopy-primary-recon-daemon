#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

#include "core/divergence.hpp"
#include "core/order_state_store.hpp"
#include "core/reconciler.hpp"
#include "ingest/spsc_ring.hpp"
#include "util/arena.hpp"

namespace {

using ExecRing = ingest::SpscRing<core::ExecEvent, 1u << 16>;

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

    explicit Harness(std::size_t capacity_hint = 128u)
        : primary_ring(std::make_unique<ExecRing>()),
          dropcopy_ring(std::make_unique<ExecRing>()),
          divergence_ring(std::make_unique<core::DivergenceRing>()),
          seq_gap_ring(std::make_unique<core::SequenceGapRing>()),
          store(arena, capacity_hint),
          reconciler(stop_flag,
                     *primary_ring,
                     *dropcopy_ring,
                     store,
                     counters,
                     *divergence_ring,
                     *seq_gap_ring) {}
};

class ReconcilerLogicTest : public ::testing::Test {
protected:
    core::ExecEvent make_event(core::Source src,
                               core::OrdStatus status,
                               std::int64_t cum_qty,
                               std::int64_t price_micro,
                               std::uint64_t ts,
                               const char* clord_id = "CID1",
                               const char* exec_id = "EXEC1") {
        core::ExecEvent ev{};
        ev.source = src;
        ev.seq_num = seq_seed_++;
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

    std::vector<core::Divergence> pop_divergences(core::DivergenceRing& ring) {
        std::vector<core::Divergence> out;
        core::Divergence div{};
        while (ring.try_pop(div)) {
            out.push_back(div);
        }
        return out;
    }

private:
    std::uint64_t seq_seed_{1};
};

TEST_F(ReconcilerLogicTest, MatchingViewsNoDivergence) {
    Harness h;
    auto internal = make_event(core::Source::Primary, core::OrdStatus::Working, 10, 100, 10, "CIDM", "INT1");
    auto dropcopy = make_event(core::Source::DropCopy, core::OrdStatus::Working, 10, 100, 12, "CIDM", "DC1");

    h.reconciler.process_event_for_test(internal);
    h.reconciler.process_event_for_test(dropcopy);

    core::Divergence div{};
    EXPECT_FALSE(h.divergence_ring->try_pop(div)) << "Unexpected divergence for matching internal/dropcopy views";
    EXPECT_EQ(h.counters.divergence_total, 0);
    EXPECT_EQ(h.counters.internal_events, 1);
    EXPECT_EQ(h.counters.dropcopy_events, 1);
    EXPECT_EQ(h.counters.divergence_ring_drops, 0);
}

TEST_F(ReconcilerLogicTest, MissingFillEmitted) {
    Harness h;
    auto internal = make_event(core::Source::Primary, core::OrdStatus::Working, 10, 100, 10, "CID1", "INT1");
    auto dropcopy = make_event(core::Source::DropCopy, core::OrdStatus::Filled, 20, 105, 12, "CID1", "DC1");

    h.reconciler.process_event_for_test(internal);
    h.reconciler.process_event_for_test(dropcopy);

    core::Divergence div{};
    ASSERT_TRUE(h.divergence_ring->try_pop(div)) << "Expected divergence for missing fill";
    EXPECT_EQ(div.type, core::DivergenceType::MissingFill);
    EXPECT_EQ(h.counters.divergence_total, 1);
    EXPECT_EQ(h.counters.divergence_missing_fill, 1);
    EXPECT_EQ(h.counters.divergence_ring_drops, 0);
}

TEST_F(ReconcilerLogicTest, PhantomOrderEmitted) {
    Harness h;
    auto dropcopy = make_event(core::Source::DropCopy, core::OrdStatus::New, 0, 100, 10, "CID2", "DC2");
    h.reconciler.process_event_for_test(dropcopy);

    core::Divergence div{};
    ASSERT_TRUE(h.divergence_ring->try_pop(div)) << "Expected phantom order divergence for dropcopy-only event";
    EXPECT_EQ(div.type, core::DivergenceType::PhantomOrder);
    EXPECT_EQ(h.counters.divergence_total, 1);
    EXPECT_EQ(h.counters.divergence_phantom, 1);
}

TEST_F(ReconcilerLogicTest, StateAndQuantityMismatches) {
    Harness h;
    auto internal_state = make_event(core::Source::Primary, core::OrdStatus::PartiallyFilled, 50, 100, 10, "CID3", "INT3");
    auto dropcopy_state = make_event(core::Source::DropCopy, core::OrdStatus::Filled, 50, 100, 12, "CID3", "DC3");

    h.reconciler.process_event_for_test(internal_state);
    h.reconciler.process_event_for_test(dropcopy_state);

    core::Divergence first{};
    ASSERT_TRUE(h.divergence_ring->try_pop(first)) << "Expected divergence for mismatched states";
    EXPECT_EQ(first.type, core::DivergenceType::StateMismatch);
    EXPECT_EQ(h.counters.divergence_state_mismatch, 1);

    auto internal_qty = make_event(core::Source::Primary, core::OrdStatus::Filled, 10, 100, 20, "CID4", "INT4");
    auto dropcopy_qty = make_event(core::Source::DropCopy, core::OrdStatus::Filled, 20, 100, 22, "CID4", "DC4");

    h.reconciler.process_event_for_test(internal_qty);
    h.reconciler.process_event_for_test(dropcopy_qty);

    core::Divergence second{};
    ASSERT_TRUE(h.divergence_ring->try_pop(second)) << "Expected divergence for quantity mismatch";
    EXPECT_EQ(second.type, core::DivergenceType::QuantityMismatch);
    EXPECT_EQ(h.counters.divergence_quantity_mismatch, 1);
    EXPECT_EQ(h.counters.divergence_total, 2);
}

TEST_F(ReconcilerLogicTest, IntegrationStyleRingDraining) {
    Harness h;
    auto p1 = make_event(core::Source::Primary, core::OrdStatus::New, 0, 100, 10, "CID5", "P1");
    auto p2 = make_event(core::Source::Primary, core::OrdStatus::Working, 0, 100, 11, "CID5", "P2");
    auto p3 = make_event(core::Source::Primary, core::OrdStatus::PartiallyFilled, 50, 100, 12, "CID5", "P3");
    auto d1 = make_event(core::Source::DropCopy, core::OrdStatus::PartiallyFilled, 50, 100, 13, "CID5", "D1");
    auto d2 = make_event(core::Source::DropCopy, core::OrdStatus::Filled, 100, 100, 14, "CID5", "D2");

    ASSERT_TRUE(h.primary_ring->try_push(p1));
    ASSERT_TRUE(h.primary_ring->try_push(p2));
    ASSERT_TRUE(h.primary_ring->try_push(p3));
    ASSERT_TRUE(h.dropcopy_ring->try_push(d1));
    ASSERT_TRUE(h.dropcopy_ring->try_push(d2));

    core::ExecEvent ev{};
    core::ExecEvent dc{};
    while (true) {
        const bool cp = h.primary_ring->try_pop(ev);
        const bool cd = h.dropcopy_ring->try_pop(dc);
        if (!cp && !cd) break;
        if (cp) h.reconciler.process_event_for_test(ev);
        if (cd) h.reconciler.process_event_for_test(dc);
    }

    const auto divergences = pop_divergences(*h.divergence_ring);
    const bool saw_missing_fill = std::any_of(divergences.begin(), divergences.end(), [](const core::Divergence& d) {
        return d.type == core::DivergenceType::MissingFill;
    });

    SCOPED_TRACE("Divergence count=" + std::to_string(divergences.size()));
    EXPECT_TRUE(saw_missing_fill);
    EXPECT_EQ(divergences.size(), h.counters.divergence_total);
    EXPECT_GE(h.counters.divergence_total, 1);
    EXPECT_EQ(h.counters.internal_events, 3);
    EXPECT_EQ(h.counters.dropcopy_events, 2);
    EXPECT_EQ(h.counters.divergence_ring_drops, 0);
}

} // namespace
