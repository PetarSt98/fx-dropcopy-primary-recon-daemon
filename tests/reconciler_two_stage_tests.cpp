#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
#include <memory>

#include "core/divergence.hpp"
#include "core/order_state_store.hpp"
#include "core/reconciler.hpp"
#include "core/recon_config.hpp"
#include "ingest/spsc_ring.hpp"
#include "util/arena.hpp"
#include "util/wheel_timer.hpp"

namespace {

using ExecRing = ingest::SpscRing<core::ExecEvent, 1u << 16>;

struct TwoStageHarness {
    std::atomic<bool> stop_flag{false};
    std::unique_ptr<ExecRing> primary_ring;
    std::unique_ptr<ExecRing> dropcopy_ring;
    std::unique_ptr<core::DivergenceRing> divergence_ring;
    std::unique_ptr<core::SequenceGapRing> seq_gap_ring;
    util::Arena arena{util::Arena::default_capacity_bytes};
    core::OrderStateStore store;
    core::ReconCounters counters{};
    util::WheelTimer timer_wheel{0};
    core::ReconConfig config{};
    std::unique_ptr<core::Reconciler> reconciler;

    explicit TwoStageHarness(std::size_t capacity_hint = 128u)
        : primary_ring(std::make_unique<ExecRing>()),
          dropcopy_ring(std::make_unique<ExecRing>()),
          divergence_ring(std::make_unique<core::DivergenceRing>()),
          seq_gap_ring(std::make_unique<core::SequenceGapRing>()),
          store(arena, capacity_hint)
    {
        reconciler = std::make_unique<core::Reconciler>(
            stop_flag,
            *primary_ring,
            *dropcopy_ring,
            store,
            counters,
            *divergence_ring,
            *seq_gap_ring,
            &timer_wheel,
            config);
    }
};

class ReconcilerTwoStageTest : public ::testing::Test {
protected:
    std::uint64_t seq_seed_{1};

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
        ev.ingest_tsc = ts;
        ev.set_clord_id(clord_id, std::strlen(clord_id));
        ev.set_exec_id(exec_id, std::strlen(exec_id));
        return ev;
    }
};

// Reconciler_NewConstructor_Compiles - New constructor works
TEST_F(ReconcilerTwoStageTest, NewConstructorCompiles) {
    TwoStageHarness h;
    EXPECT_NE(h.reconciler.get(), nullptr);
}

// Reconciler_BothSidesSeen_TrueWhenBothTrue - Helper returns correct value
TEST_F(ReconcilerTwoStageTest, BothSidesSeen_TrueWhenBothTrue) {
    core::OrderState os{};
    os.seen_internal = true;
    os.seen_dropcopy = true;

    EXPECT_TRUE(core::Reconciler::both_sides_seen(os));
}

// Reconciler_BothSidesSeen_FalseWhenOnlyPrimary - Helper returns false
TEST_F(ReconcilerTwoStageTest, BothSidesSeen_FalseWhenOnlyPrimary) {
    core::OrderState os{};
    os.seen_internal = true;
    os.seen_dropcopy = false;

    EXPECT_FALSE(core::Reconciler::both_sides_seen(os));
}

// Reconciler_BothSidesSeen_FalseWhenOnlyDropcopy - Helper returns false
TEST_F(ReconcilerTwoStageTest, BothSidesSeen_FalseWhenOnlyDropcopy) {
    core::OrderState os{};
    os.seen_internal = false;
    os.seen_dropcopy = true;

    EXPECT_FALSE(core::Reconciler::both_sides_seen(os));
}

// Reconciler_BothSidesSeen_FalseWhenNeitherSeen - Helper returns false
TEST_F(ReconcilerTwoStageTest, BothSidesSeen_FalseWhenNeitherSeen) {
    core::OrderState os{};
    os.seen_internal = false;
    os.seen_dropcopy = false;

    EXPECT_FALSE(core::Reconciler::both_sides_seen(os));
}

// Reconciler_IsGapSuppressed_FalseWhenDisabled - Config flag respected
TEST_F(ReconcilerTwoStageTest, IsGapSuppressed_FalseWhenDisabled) {
    TwoStageHarness h;
    h.config.enable_gap_suppression = false;

    // Recreate reconciler with disabled gap suppression
    h.reconciler = std::make_unique<core::Reconciler>(
        h.stop_flag,
        *h.primary_ring,
        *h.dropcopy_ring,
        h.store,
        h.counters,
        *h.divergence_ring,
        *h.seq_gap_ring,
        &h.timer_wheel,
        h.config);

    core::OrderState os{};
    os.gap_suppression_epoch = 1;  // Would normally trigger suppression

    EXPECT_FALSE(h.reconciler->is_gap_suppressed(os));
}

// Reconciler_IsGapSuppressed_FalseWhenNoGapOpen - Returns false when no gap
TEST_F(ReconcilerTwoStageTest, IsGapSuppressed_FalseWhenNoGapOpen) {
    TwoStageHarness h;

    core::OrderState os{};
    os.gap_suppression_epoch = 0;  // No gap flagged

    EXPECT_FALSE(h.reconciler->is_gap_suppressed(os));
}

// Reconciler_EnterGracePeriod_SetsState - Verify state transition
TEST_F(ReconcilerTwoStageTest, EnterGracePeriod_SetsState) {
    TwoStageHarness h;

    core::OrderState os{};
    os.key = 12345;
    os.recon_state = core::ReconState::Unknown;

    core::MismatchMask mismatch{};
    mismatch.set(core::MismatchMask::STATUS);

    const std::uint64_t now_tsc = 1'000'000'000;

    h.reconciler->enter_grace_period(os, mismatch, now_tsc);

    EXPECT_EQ(os.recon_state, core::ReconState::InGrace);
    EXPECT_EQ(os.current_mismatch.bits(), mismatch.bits());
    EXPECT_EQ(os.mismatch_first_seen_tsc, now_tsc);
    EXPECT_EQ(h.counters.mismatch_observed, 1u);
}

// Reconciler_ExitGracePeriod_ClearsState - Verify state cleared
TEST_F(ReconcilerTwoStageTest, ExitGracePeriod_ClearsState) {
    TwoStageHarness h;

    core::OrderState os{};
    os.key = 12345;
    os.recon_state = core::ReconState::InGrace;
    os.current_mismatch.set(core::MismatchMask::STATUS);

    const std::uint64_t now_tsc = 2'000'000'000;

    h.reconciler->exit_grace_period(os, now_tsc);

    EXPECT_EQ(os.recon_state, core::ReconState::Matched);
    EXPECT_EQ(os.current_mismatch.bits(), 0u);
    EXPECT_EQ(h.counters.false_positive_avoided, 1u);
    EXPECT_EQ(h.counters.orders_matched, 1u);
}

// Reconciler_OnDeadlineExpired_SkipsStaleTimer - Generation mismatch handled
TEST_F(ReconcilerTwoStageTest, OnDeadlineExpired_SkipsStaleTimer) {
    TwoStageHarness h;

    // Create an order in the store
    auto ev = make_event(core::Source::Primary, core::OrdStatus::Working, 10, 100, 10, "CID1", "EX1");
    h.reconciler->process_event_for_test(ev);

    // Find the order
    core::OrderKey key = core::make_order_key(ev);
    core::OrderState* os = h.store.find(key);
    ASSERT_NE(os, nullptr);

    // Set up the order state with a timer generation
    os->timer_generation = 5;

    // Call with stale generation (should be skipped)
    h.reconciler->on_grace_deadline_expired(key, 3);  // Stale gen = 3

    EXPECT_EQ(h.counters.stale_timers_skipped, 1u);
}

// Reconciler_NewConstructor_WithNullTimerWheel - Works with nullptr timer wheel
TEST_F(ReconcilerTwoStageTest, NewConstructorWithNullTimerWheel) {
    std::atomic<bool> stop_flag{false};
    // Allocate large ring buffers on heap to avoid stack overflow
    auto primary_ring = std::make_unique<ExecRing>();
    auto dropcopy_ring = std::make_unique<ExecRing>();
    auto divergence_ring = std::make_unique<core::DivergenceRing>();
    auto seq_gap_ring = std::make_unique<core::SequenceGapRing>();
    util::Arena arena{util::Arena::default_capacity_bytes};
    core::OrderStateStore store{arena, 128};
    core::ReconCounters counters{};
    core::ReconConfig config{};

    // Create reconciler with nullptr timer wheel
    core::Reconciler reconciler(
        stop_flag,
        *primary_ring,
        *dropcopy_ring,
        store,
        counters,
        *divergence_ring,
        *seq_gap_ring,
        nullptr,  // No timer wheel
        config);

    // Should compile and construct without issues
    SUCCEED();
}

// Reconciler_BackwardCompatibility - Old constructor still works
TEST_F(ReconcilerTwoStageTest, BackwardCompatibility) {
    std::atomic<bool> stop_flag{false};
    // Allocate large ring buffers on heap to avoid stack overflow
    auto primary_ring = std::make_unique<ExecRing>();
    auto dropcopy_ring = std::make_unique<ExecRing>();
    auto divergence_ring = std::make_unique<core::DivergenceRing>();
    auto seq_gap_ring = std::make_unique<core::SequenceGapRing>();
    util::Arena arena{util::Arena::default_capacity_bytes};
    core::OrderStateStore store{arena, 128};
    core::ReconCounters counters{};

    // Use old constructor (backward compatibility)
    core::Reconciler reconciler(
        stop_flag,
        *primary_ring,
        *dropcopy_ring,
        store,
        counters,
        *divergence_ring,
        *seq_gap_ring);

    // Should compile and construct without issues
    SUCCEED();
}

// classify_divergence_type tests
TEST_F(ReconcilerTwoStageTest, ClassifyDivergenceType_MissingDropCopy) {
    core::OrderState os{};
    os.seen_internal = true;
    os.seen_dropcopy = false;

    core::MismatchMask mismatch{};
    mismatch.set(core::MismatchMask::EXISTENCE);

    EXPECT_EQ(core::classify_divergence_type(os, mismatch), core::DivergenceType::MissingDropCopy);
}

TEST_F(ReconcilerTwoStageTest, ClassifyDivergenceType_PhantomOrder) {
    core::OrderState os{};
    os.seen_internal = false;
    os.seen_dropcopy = true;

    core::MismatchMask mismatch{};
    mismatch.set(core::MismatchMask::EXISTENCE);

    EXPECT_EQ(core::classify_divergence_type(os, mismatch), core::DivergenceType::PhantomOrder);
}

TEST_F(ReconcilerTwoStageTest, ClassifyDivergenceType_StateMismatch) {
    core::OrderState os{};
    os.seen_internal = true;
    os.seen_dropcopy = true;

    core::MismatchMask mismatch{};
    mismatch.set(core::MismatchMask::STATUS);

    EXPECT_EQ(core::classify_divergence_type(os, mismatch), core::DivergenceType::StateMismatch);
}

TEST_F(ReconcilerTwoStageTest, ClassifyDivergenceType_QuantityMismatch) {
    core::OrderState os{};
    os.seen_internal = true;
    os.seen_dropcopy = true;

    core::MismatchMask mismatch{};
    mismatch.set(core::MismatchMask::CUM_QTY);

    EXPECT_EQ(core::classify_divergence_type(os, mismatch), core::DivergenceType::QuantityMismatch);
}

// Verify noexcept guarantees
TEST_F(ReconcilerTwoStageTest, HelperMethodsAreNoexcept) {
    TwoStageHarness h;
    core::OrderState os{};
    core::MismatchMask mismatch{};

    // These should all be noexcept
    static_assert(noexcept(core::Reconciler::both_sides_seen(os)),
                  "both_sides_seen must be noexcept");
    static_assert(noexcept(h.reconciler->is_gap_suppressed(os)),
                  "is_gap_suppressed must be noexcept");
    static_assert(noexcept(h.reconciler->enter_grace_period(os, mismatch, 0)),
                  "enter_grace_period must be noexcept");
    static_assert(noexcept(h.reconciler->exit_grace_period(os, 0)),
                  "exit_grace_period must be noexcept");
    static_assert(noexcept(h.reconciler->on_grace_deadline_expired(0, 0)),
                  "on_grace_deadline_expired must be noexcept");
    static_assert(noexcept(h.reconciler->emit_confirmed_divergence(os, mismatch, 0)),
                  "emit_confirmed_divergence must be noexcept");
}

} // namespace
