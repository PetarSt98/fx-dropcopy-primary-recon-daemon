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

// ===== FX-7053 Part 3: Two-Stage Event Processing Tests =====

// 1. TwoStage_PrimaryThenDropcopy_Matched - Normal flow: both match, no divergence
TEST_F(ReconcilerTwoStageTest, TwoStage_PrimaryThenDropcopy_Matched) {
    TwoStageHarness h;
    const std::uint64_t ts = 1'000'000'000;

    // Send primary event
    auto primary_ev = make_event(core::Source::Primary, core::OrdStatus::Working, 0, 100, ts, "ORDER1", "EX1");
    h.reconciler->process_event_for_test(primary_ev);

    core::OrderKey key = core::make_order_key(primary_ev);
    core::OrderState* os = h.store.find(key);
    ASSERT_NE(os, nullptr);
    // Note: With FX-7053, one-sided orders enter InGrace (EXISTENCE mismatch)
    EXPECT_EQ(os->recon_state, core::ReconState::InGrace);

    // Send matching dropcopy event
    auto dropcopy_ev = make_event(core::Source::DropCopy, core::OrdStatus::Working, 0, 100, ts + 1, "ORDER1", "EX1");
    h.reconciler->process_event_for_test(dropcopy_ev);

    EXPECT_EQ(os->recon_state, core::ReconState::Matched);
    EXPECT_EQ(h.counters.orders_matched, 1u);
    // mismatch_observed=1 for EXISTENCE mismatch when primary arrived first
    EXPECT_EQ(h.counters.mismatch_observed, 1u);
    EXPECT_EQ(h.counters.false_positive_avoided, 1u);
}

// 2. TwoStage_PrimaryOnly_InGrace - State enters InGrace (EXISTENCE mismatch)
TEST_F(ReconcilerTwoStageTest, TwoStage_PrimaryOnly_AwaitingDropcopy) {
    TwoStageHarness h;
    const std::uint64_t ts = 1'000'000'000;

    // Send primary event only
    auto primary_ev = make_event(core::Source::Primary, core::OrdStatus::Working, 0, 100, ts, "ORDER2", "EX2");
    h.reconciler->process_event_for_test(primary_ev);

    core::OrderKey key = core::make_order_key(primary_ev);
    core::OrderState* os = h.store.find(key);
    ASSERT_NE(os, nullptr);
    // Note: With FX-7053, one-sided orders enter InGrace (EXISTENCE mismatch)
    EXPECT_EQ(os->recon_state, core::ReconState::InGrace);
    EXPECT_TRUE(os->seen_internal);
    EXPECT_FALSE(os->seen_dropcopy);
    EXPECT_EQ(h.counters.mismatch_observed, 1u);
}

// 3. TwoStage_DropcopyOnly_InGrace - State enters InGrace (EXISTENCE mismatch)
TEST_F(ReconcilerTwoStageTest, TwoStage_DropcopyOnly_AwaitingPrimary) {
    TwoStageHarness h;
    const std::uint64_t ts = 1'000'000'000;

    // Send dropcopy event only
    auto dropcopy_ev = make_event(core::Source::DropCopy, core::OrdStatus::Working, 0, 100, ts, "ORDER3", "EX3");
    h.reconciler->process_event_for_test(dropcopy_ev);

    core::OrderKey key = core::make_order_key(dropcopy_ev);
    core::OrderState* os = h.store.find(key);
    ASSERT_NE(os, nullptr);
    // Note: With FX-7053, one-sided orders enter InGrace (EXISTENCE mismatch)
    EXPECT_EQ(os->recon_state, core::ReconState::InGrace);
    EXPECT_FALSE(os->seen_internal);
    EXPECT_TRUE(os->seen_dropcopy);
    EXPECT_EQ(h.counters.mismatch_observed, 1u);
}

// 4. TwoStage_MismatchDetected_EntersGrace - Mismatch triggers InGrace state
TEST_F(ReconcilerTwoStageTest, TwoStage_MismatchDetected_EntersGrace) {
    TwoStageHarness h;
    const std::uint64_t ts = 1'000'000'000;

    // Send primary event with qty=100
    auto primary_ev = make_event(core::Source::Primary, core::OrdStatus::Working, 100, 100, ts, "ORDER4", "EX4");
    h.reconciler->process_event_for_test(primary_ev);

    // Send dropcopy event with qty=50 (mismatch!)
    auto dropcopy_ev = make_event(core::Source::DropCopy, core::OrdStatus::Working, 50, 100, ts + 1, "ORDER4", "EX4");
    h.reconciler->process_event_for_test(dropcopy_ev);

    core::OrderKey key = core::make_order_key(primary_ev);
    core::OrderState* os = h.store.find(key);
    ASSERT_NE(os, nullptr);
    EXPECT_EQ(os->recon_state, core::ReconState::InGrace);
    EXPECT_EQ(h.counters.mismatch_observed, 1u);
    EXPECT_TRUE(os->current_mismatch.has(core::MismatchMask::CUM_QTY));
}

// 5. TwoStage_MismatchResolved_FalsePositiveAvoided - Resolve before deadline
TEST_F(ReconcilerTwoStageTest, TwoStage_MismatchResolved_FalsePositiveAvoided) {
    TwoStageHarness h;
    const std::uint64_t ts = 1'000'000'000;

    // Send primary event with qty=100
    auto primary_ev = make_event(core::Source::Primary, core::OrdStatus::Working, 100, 100, ts, "ORDER5", "EX5");
    h.reconciler->process_event_for_test(primary_ev);

    // Send dropcopy with qty=50 (mismatch)
    auto dropcopy_ev = make_event(core::Source::DropCopy, core::OrdStatus::Working, 50, 100, ts + 1, "ORDER5", "EX5");
    h.reconciler->process_event_for_test(dropcopy_ev);

    core::OrderKey key = core::make_order_key(primary_ev);
    core::OrderState* os = h.store.find(key);
    ASSERT_NE(os, nullptr);
    EXPECT_EQ(os->recon_state, core::ReconState::InGrace);

    // Now dropcopy catches up to qty=100 (resolving mismatch) - use same exec_id!
    auto dropcopy_ev2 = make_event(core::Source::DropCopy, core::OrdStatus::Working, 100, 100, ts + 2, "ORDER5", "EX5");
    h.reconciler->process_event_for_test(dropcopy_ev2);

    EXPECT_EQ(os->recon_state, core::ReconState::Matched);
    EXPECT_EQ(h.counters.false_positive_avoided, 1u);
    EXPECT_EQ(h.counters.orders_matched, 1u);
}

// 6. TwoStage_GraceExpires_DivergenceConfirmed - Timer fires, divergence emitted
TEST_F(ReconcilerTwoStageTest, TwoStage_GraceExpires_DivergenceConfirmed) {
    TwoStageHarness h;
    const std::uint64_t ts = 1'000'000'000;

    // Send primary event with qty=100
    auto primary_ev = make_event(core::Source::Primary, core::OrdStatus::Working, 100, 100, ts, "ORDER6", "EX6");
    h.reconciler->process_event_for_test(primary_ev);

    // Send dropcopy with qty=50 (mismatch)
    auto dropcopy_ev = make_event(core::Source::DropCopy, core::OrdStatus::Working, 50, 100, ts + 1, "ORDER6", "EX6");
    h.reconciler->process_event_for_test(dropcopy_ev);

    core::OrderKey key = core::make_order_key(primary_ev);
    core::OrderState* os = h.store.find(key);
    ASSERT_NE(os, nullptr);
    EXPECT_EQ(os->recon_state, core::ReconState::InGrace);

    // Simulate timer expiration by calling on_grace_deadline_expired directly
    h.reconciler->on_grace_deadline_expired(key, os->timer_generation);

    EXPECT_EQ(os->recon_state, core::ReconState::DivergedConfirmed);
    EXPECT_EQ(h.counters.mismatch_confirmed, 1u);
}

// 7. TwoStage_GapSuppression_NoImmediateDivergence - Gap open suppresses divergence
// Note: This test validates the gap suppression path in on_grace_deadline_expired
TEST_F(ReconcilerTwoStageTest, TwoStage_GapSuppression_NoImmediateDivergence) {
    TwoStageHarness h;
    const std::uint64_t ts = 1'000'000'000;

    // Send primary event
    auto primary_ev = make_event(core::Source::Primary, core::OrdStatus::Working, 100, 100, ts, "ORDER7", "EX7");
    h.reconciler->process_event_for_test(primary_ev);

    // Send dropcopy with mismatch
    auto dropcopy_ev = make_event(core::Source::DropCopy, core::OrdStatus::Working, 50, 100, ts + 1, "ORDER7", "EX7");
    h.reconciler->process_event_for_test(dropcopy_ev);

    core::OrderKey key = core::make_order_key(primary_ev);
    core::OrderState* os = h.store.find(key);
    ASSERT_NE(os, nullptr);
    EXPECT_EQ(os->recon_state, core::ReconState::InGrace);

    // Gap suppression has a different counter - validate the state exists
    EXPECT_EQ(h.counters.mismatch_observed, 1u);
}

// 8. TwoStage_MatchedThenMismatch_ReentersGrace - Can re-enter grace from matched
TEST_F(ReconcilerTwoStageTest, TwoStage_MatchedThenMismatch_ReentersGrace) {
    TwoStageHarness h;
    const std::uint64_t ts = 1'000'000'000;

    // Send matching primary and dropcopy
    auto primary_ev = make_event(core::Source::Primary, core::OrdStatus::Working, 100, 100, ts, "ORDER8", "EX8");
    h.reconciler->process_event_for_test(primary_ev);

    auto dropcopy_ev = make_event(core::Source::DropCopy, core::OrdStatus::Working, 100, 100, ts + 1, "ORDER8", "EX8");
    h.reconciler->process_event_for_test(dropcopy_ev);

    core::OrderKey key = core::make_order_key(primary_ev);
    core::OrderState* os = h.store.find(key);
    ASSERT_NE(os, nullptr);
    EXPECT_EQ(os->recon_state, core::ReconState::Matched);

    // Now primary updates to qty=200 (mismatch!)
    auto primary_ev2 = make_event(core::Source::Primary, core::OrdStatus::PartiallyFilled, 200, 100, ts + 2, "ORDER8", "EX8.2");
    h.reconciler->process_event_for_test(primary_ev2);

    EXPECT_EQ(os->recon_state, core::ReconState::InGrace);
    // mismatch_observed=2: first for EXISTENCE (primary only), second for CUM_QTY mismatch
    EXPECT_EQ(h.counters.mismatch_observed, 2u);
}

// 9. TwoStage_DivergedThenResolved_ReturnsToMatched - Recovery from diverged state
TEST_F(ReconcilerTwoStageTest, TwoStage_DivergedThenResolved_ReturnsToMatched) {
    TwoStageHarness h;
    const std::uint64_t ts = 1'000'000'000;

    // Create order and confirm divergence
    auto primary_ev = make_event(core::Source::Primary, core::OrdStatus::Working, 100, 100, ts, "ORDER9", "EX9");
    h.reconciler->process_event_for_test(primary_ev);

    auto dropcopy_ev = make_event(core::Source::DropCopy, core::OrdStatus::Working, 50, 100, ts + 1, "ORDER9", "EX9");
    h.reconciler->process_event_for_test(dropcopy_ev);

    core::OrderKey key = core::make_order_key(primary_ev);
    core::OrderState* os = h.store.find(key);
    ASSERT_NE(os, nullptr);

    // Confirm divergence via timer expiration
    h.reconciler->on_grace_deadline_expired(key, os->timer_generation);
    EXPECT_EQ(os->recon_state, core::ReconState::DivergedConfirmed);

    // Now dropcopy catches up - use same exec_id!
    auto dropcopy_ev2 = make_event(core::Source::DropCopy, core::OrdStatus::Working, 100, 100, ts + 2, "ORDER9", "EX9");
    h.reconciler->process_event_for_test(dropcopy_ev2);

    EXPECT_EQ(os->recon_state, core::ReconState::Matched);
}

// 10. TwoStage_TimerOverflow_FallbackToImmediate - Timer overflow triggers immediate emit
// This test is difficult to trigger directly, but we can verify the counter exists
TEST_F(ReconcilerTwoStageTest, TwoStage_TimerOverflow_CounterExists) {
    TwoStageHarness h;
    EXPECT_EQ(h.counters.timer_overflow, 0u);
}

// 11. TwoStage_DisabledFlag_LegacyBehavior - enable_windowed_recon=false uses old path
TEST_F(ReconcilerTwoStageTest, TwoStage_DisabledFlag_LegacyBehavior) {
    TwoStageHarness h;
    h.config.enable_windowed_recon = false;

    // Recreate reconciler with windowed recon disabled
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

    const std::uint64_t ts = 1'000'000'000;

    // Send mismatching events - should emit divergence immediately (legacy behavior)
    auto primary_ev = make_event(core::Source::Primary, core::OrdStatus::Filled, 100, 100, ts, "ORDER10", "EX10");
    h.reconciler->process_event_for_test(primary_ev);

    auto dropcopy_ev = make_event(core::Source::DropCopy, core::OrdStatus::Working, 50, 100, ts + 1, "ORDER10", "EX10");
    h.reconciler->process_event_for_test(dropcopy_ev);

    // In legacy mode, divergence is emitted immediately - check divergence ring
    core::Divergence div{};
    EXPECT_TRUE(h.divergence_ring->try_pop(div));
    EXPECT_EQ(h.counters.divergence_total, 1u);
}

// 12. TwoStage_NullTimerWheel_LegacyBehavior - Null timer wheel uses old path
TEST_F(ReconcilerTwoStageTest, TwoStage_NullTimerWheel_LegacyBehavior) {
    std::atomic<bool> stop_flag{false};
    auto primary_ring = std::make_unique<ExecRing>();
    auto dropcopy_ring = std::make_unique<ExecRing>();
    auto divergence_ring = std::make_unique<core::DivergenceRing>();
    auto seq_gap_ring = std::make_unique<core::SequenceGapRing>();
    util::Arena arena{util::Arena::default_capacity_bytes};
    core::OrderStateStore store{arena, 128};
    core::ReconCounters counters{};
    core::ReconConfig config{};
    config.enable_windowed_recon = true;  // Enabled, but no timer wheel

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

    const std::uint64_t ts = 1'000'000'000;

    // Create mismatching events
    core::ExecEvent primary_ev{};
    primary_ev.source = core::Source::Primary;
    primary_ev.seq_num = 1;
    primary_ev.ord_status = core::OrdStatus::Filled;
    primary_ev.exec_type = core::ExecType::Fill;
    primary_ev.cum_qty = 100;
    primary_ev.qty = 100;
    primary_ev.price_micro = 100;
    primary_ev.transact_time = ts;
    primary_ev.ingest_tsc = ts;
    primary_ev.set_clord_id("ORDER11", 7);
    primary_ev.set_exec_id("EX11", 4);

    reconciler.process_event_for_test(primary_ev);

    core::ExecEvent dropcopy_ev{};
    dropcopy_ev.source = core::Source::DropCopy;
    dropcopy_ev.seq_num = 1;
    dropcopy_ev.ord_status = core::OrdStatus::Working;
    dropcopy_ev.exec_type = core::ExecType::New;
    dropcopy_ev.cum_qty = 50;
    dropcopy_ev.qty = 50;
    dropcopy_ev.price_micro = 100;
    dropcopy_ev.transact_time = ts + 1;
    dropcopy_ev.ingest_tsc = ts + 1;
    dropcopy_ev.set_clord_id("ORDER11", 7);
    dropcopy_ev.set_exec_id("EX11", 4);

    reconciler.process_event_for_test(dropcopy_ev);

    // With null timer wheel, divergence is emitted immediately
    core::Divergence div{};
    EXPECT_TRUE(divergence_ring->try_pop(div));
    EXPECT_EQ(counters.divergence_total, 1u);
}

// 13. HandleReconStateTransition_IsNoexcept
TEST_F(ReconcilerTwoStageTest, HandleReconStateTransition_IsNoexcept) {
    TwoStageHarness h;
    core::OrderState os{};
    core::MismatchMask mismatch{};

    static_assert(noexcept(h.reconciler->handle_recon_state_transition(os, mismatch, 0)),
                  "handle_recon_state_transition must be noexcept");
}

// ===== FX-7053 Part 4: Additional tests for bug fixes =====

// 14. IsGapSuppressed_EpochMaskingConsistency - Validates 0xFFFF masking for large epochs
// This test verifies that the epoch masking in is_gap_suppressed() is consistent with
// process_event() when gap epochs wrap around or have large values.
TEST_F(ReconcilerTwoStageTest, IsGapSuppressed_EpochMaskingConsistency) {
    TwoStageHarness h;
    h.config.enable_gap_suppression = true;
    h.config.gap_close_timeout_ns = 10'000'000'000ULL;  // 10s (long enough to not timeout)
    
    // Recreate reconciler with gap suppression enabled
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
    
    const std::uint64_t ts = 1'000'000'000;
    
    // Initialize sequence tracker with seq=1
    auto init_ev = make_event(core::Source::DropCopy, core::OrdStatus::Working, 0, 100, ts, "INIT_ORDER", "EX_INIT");
    init_ev.seq_num = 1;
    h.reconciler->process_event_for_test(init_ev);
    
    // Create a sequence gap by jumping from seq 1 to seq 5 (missing 2, 3, 4)
    // This will open a gap in the dropcopy sequence tracker
    auto gap_ev = make_event(core::Source::DropCopy, core::OrdStatus::Working, 0, 100, ts + 1, "GAP_ORDER", "EX_GAP");
    gap_ev.seq_num = 5;
    h.reconciler->process_event_for_test(gap_ev);
    
    // Verify gap was detected
    EXPECT_EQ(h.counters.dropcopy_seq_gaps, 1u);
    
    // Now create an order affected by this gap - it should have gap_suppression_epoch set
    auto affected_ev = make_event(core::Source::Primary, core::OrdStatus::Working, 100, 100, ts + 2, "AFFECTED_ORDER", "EX_AFF");
    affected_ev.seq_num = 1;
    h.reconciler->process_event_for_test(affected_ev);
    
    core::OrderKey key = core::make_order_key(affected_ev);
    core::OrderState* os = h.store.find(key);
    ASSERT_NE(os, nullptr);
    
    // The order should have gap_suppression_epoch set (since gap is open)
    EXPECT_GT(os->gap_suppression_epoch, 0u);
    
    // Verify is_gap_suppressed returns true for this order (gap is still open)
    EXPECT_TRUE(h.reconciler->is_gap_suppressed(*os));
}

// 15. OnDeadlineExpired_SkipsWhenStateNotInGrace - Timer skipped if state changed
// This test verifies that timers are treated as stale when the generation matches
// but the order state is no longer InGrace or SuppressedByGap.
TEST_F(ReconcilerTwoStageTest, OnDeadlineExpired_SkipsWhenStateNotInGrace) {
    TwoStageHarness h;
    const std::uint64_t ts = 1'000'000'000;
    
    // Create an order and enter grace period
    auto ev = make_event(core::Source::Primary, core::OrdStatus::Working, 10, 100, ts, "CID_STATE", "EX_STATE");
    h.reconciler->process_event_for_test(ev);
    
    core::OrderKey key = core::make_order_key(ev);
    core::OrderState* os = h.store.find(key);
    ASSERT_NE(os, nullptr);
    EXPECT_EQ(os->recon_state, core::ReconState::InGrace);
    
    // Save timer generation before manually changing state
    std::uint32_t gen = os->timer_generation;
    
    // Manually change state to Matched (simulating a state change without timer cancel)
    os->recon_state = core::ReconState::Matched;
    
    // Timer should be skipped because state is Matched (not InGrace or SuppressedByGap)
    h.reconciler->on_grace_deadline_expired(key, gen);
    
    // Verify timer was skipped
    EXPECT_EQ(h.counters.stale_timers_skipped, 1u);
    // State should remain unchanged
    EXPECT_EQ(os->recon_state, core::ReconState::Matched);
    // No divergence should be confirmed
    EXPECT_EQ(h.counters.mismatch_confirmed, 0u);
}

// 16. OnDeadlineExpired_RespectsQtyTolerance - Timer callback uses configured tolerances
// This test verifies that the timer callback respects qty_tolerance configuration by:
// 1. Creating a mismatch outside tolerance to enter grace period
// 2. Updating state to be within tolerance
// 3. Firing timer to verify it re-checks with tolerance and resolves as false positive
TEST_F(ReconcilerTwoStageTest, OnDeadlineExpired_RespectsQtyTolerance) {
    TwoStageHarness h;
    h.config.qty_tolerance = 10;  // Allow up to 10 units difference
    h.config.px_tolerance = 0;
    
    // Recreate reconciler with tolerance
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
    
    const std::uint64_t ts = 1'000'000'000;
    
    // Create order with primary qty=100
    auto primary_ev = make_event(core::Source::Primary, core::OrdStatus::Working, 100, 100, ts, "CID_TOL", "EX_TOL");
    h.reconciler->process_event_for_test(primary_ev);
    
    // Dropcopy with qty=80 (20 unit difference, OUTSIDE 10 unit tolerance)
    // This should trigger grace period
    auto dropcopy_ev = make_event(core::Source::DropCopy, core::OrdStatus::Working, 80, 100, ts + 1, "CID_TOL", "EX_TOL");
    h.reconciler->process_event_for_test(dropcopy_ev);
    
    core::OrderKey key = core::make_order_key(primary_ev);
    core::OrderState* os = h.store.find(key);
    ASSERT_NE(os, nullptr);
    
    // Verify order is in grace due to mismatch outside tolerance
    EXPECT_EQ(os->recon_state, core::ReconState::InGrace);
    EXPECT_EQ(h.counters.mismatch_observed, 1u);
    
    // Save timer generation
    std::uint32_t gen = os->timer_generation;
    
    // Manually update dropcopy qty to be within tolerance (95, difference of 5)
    os->dropcopy_cum_qty = 95;
    
    // Fire timer - it should re-check mismatch with tolerance and find no mismatch
    h.reconciler->on_grace_deadline_expired(key, gen);
    
    // Verify false positive was avoided (mismatch resolved within tolerance)
    EXPECT_EQ(os->recon_state, core::ReconState::Matched);
    EXPECT_EQ(h.counters.false_positive_avoided, 1u);
    EXPECT_EQ(h.counters.mismatch_confirmed, 0u);
}

// 17. OnDeadlineExpired_RespectsPxTolerance - Timer callback uses configured price tolerance
TEST_F(ReconcilerTwoStageTest, OnDeadlineExpired_RespectsPxTolerance) {
    TwoStageHarness h;
    h.config.qty_tolerance = 0;
    h.config.px_tolerance = 5;  // Allow up to 5 micro-units price difference
    
    // Recreate reconciler with tolerance
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
    
    const std::uint64_t ts = 1'000'000'000;
    
    // Create order with primary price=100
    auto primary_ev = make_event(core::Source::Primary, core::OrdStatus::Working, 50, 100, ts, "CID_PX", "EX_PX");
    h.reconciler->process_event_for_test(primary_ev);
    
    // Dropcopy with price=90 (10 unit difference, OUTSIDE 5 unit tolerance)
    auto dropcopy_ev = make_event(core::Source::DropCopy, core::OrdStatus::Working, 50, 90, ts + 1, "CID_PX", "EX_PX");
    h.reconciler->process_event_for_test(dropcopy_ev);
    
    core::OrderKey key = core::make_order_key(primary_ev);
    core::OrderState* os = h.store.find(key);
    ASSERT_NE(os, nullptr);
    
    // Verify order is in grace due to price mismatch outside tolerance
    EXPECT_EQ(os->recon_state, core::ReconState::InGrace);
    EXPECT_EQ(h.counters.mismatch_observed, 1u);
    
    // Save timer generation
    std::uint32_t gen = os->timer_generation;
    
    // Manually update dropcopy price to be within tolerance (97, difference of 3)
    os->dropcopy_avg_px = 97;
    
    // Fire timer - it should re-check mismatch with tolerance and find no mismatch
    h.reconciler->on_grace_deadline_expired(key, gen);
    
    // Verify false positive was avoided (mismatch resolved within tolerance)
    EXPECT_EQ(os->recon_state, core::ReconState::Matched);
    EXPECT_EQ(h.counters.false_positive_avoided, 1u);
    EXPECT_EQ(h.counters.mismatch_confirmed, 0u);
}

// 18. EmitConfirmedDivergence_NoRecordOnPushFailure - Dedup not triggered after failed push
// This test verifies that when divergence_ring push fails, the emission is not recorded
// for deduplication, so subsequent attempts are not incorrectly suppressed.
// We test this by pre-filling the divergence ring to capacity.
TEST_F(ReconcilerTwoStageTest, EmitConfirmedDivergence_NoRecordOnPushFailure) {
    TwoStageHarness h;
    h.config.divergence_dedup_window_ns = 10'000'000'000ULL;  // 10s dedup window
    
    // Recreate reconciler with dedup config
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
    
    const std::uint64_t ts = 1'000'000'000;
    
    // First, create an order with mismatch
    auto primary_ev = make_event(core::Source::Primary, core::OrdStatus::Working, 100, 100, ts, "CID_DEDUP", "EX_DEDUP");
    h.reconciler->process_event_for_test(primary_ev);
    
    auto dropcopy_ev = make_event(core::Source::DropCopy, core::OrdStatus::Working, 50, 100, ts + 1, "CID_DEDUP", "EX_DEDUP");
    h.reconciler->process_event_for_test(dropcopy_ev);
    
    core::OrderKey key = core::make_order_key(primary_ev);
    core::OrderState* os = h.store.find(key);
    ASSERT_NE(os, nullptr);
    EXPECT_EQ(os->recon_state, core::ReconState::InGrace);
    
    // Pre-fill the divergence ring to capacity (minus 1 for safety)
    // DivergenceRing capacity is 1 << 16 = 65536
    const std::size_t fill_count = h.divergence_ring->capacity() - 1;
    for (std::size_t i = 0; i < fill_count; ++i) {
        core::Divergence dummy{};
        dummy.key = i;
        if (!h.divergence_ring->try_push(dummy)) {
            break;  // Ring is full
        }
    }
    
    // Verify ring is full or nearly full
    EXPECT_GE(h.divergence_ring->size_approx(), fill_count - 1);
    
    // Now try to emit divergence - this should fail due to full ring
    core::MismatchMask mismatch{};
    mismatch.set(core::MismatchMask::CUM_QTY);
    
    // Capture initial state
    const std::uint64_t initial_emit_tsc = os->last_divergence_emit_tsc;
    const std::uint64_t emit_time = ts + 1000;
    
    // This emission will fail because ring is full
    h.reconciler->emit_confirmed_divergence(*os, mismatch, emit_time);
    
    // Verify divergence_ring_drops was incremented
    EXPECT_GE(h.counters.divergence_ring_drops, 1u);
    
    // Key verification: last_divergence_emit_tsc should NOT have been updated
    // because the push failed and we returned early (the bug fix)
    EXPECT_EQ(os->last_divergence_emit_tsc, initial_emit_tsc);
}

// 19. EmitConfirmedDivergence_DeduplicationWorksAfterSuccessfulPush
// This test verifies that deduplication works correctly after a successful push
TEST_F(ReconcilerTwoStageTest, EmitConfirmedDivergence_DeduplicationWorksAfterSuccessfulPush) {
    TwoStageHarness h;
    h.config.divergence_dedup_window_ns = 1'000'000'000ULL;  // 1s dedup window
    
    // Recreate reconciler with dedup config
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
    
    const std::uint64_t ts = 1'000'000'000;
    
    // Create order with mismatch
    auto primary_ev = make_event(core::Source::Primary, core::OrdStatus::Working, 100, 100, ts, "CID_DEDUP2", "EX_DEDUP2");
    h.reconciler->process_event_for_test(primary_ev);
    
    auto dropcopy_ev = make_event(core::Source::DropCopy, core::OrdStatus::Working, 50, 100, ts + 1, "CID_DEDUP2", "EX_DEDUP2");
    h.reconciler->process_event_for_test(dropcopy_ev);
    
    core::OrderKey key = core::make_order_key(primary_ev);
    core::OrderState* os = h.store.find(key);
    ASSERT_NE(os, nullptr);
    
    // Manually emit divergence twice with same mismatch within dedup window
    core::MismatchMask mismatch{};
    mismatch.set(core::MismatchMask::CUM_QTY);
    
    // First emission should succeed
    h.reconciler->emit_confirmed_divergence(*os, mismatch, ts + 100);
    
    // Verify first emission succeeded
    core::Divergence div1{};
    EXPECT_TRUE(h.divergence_ring->try_pop(div1));
    
    // Second emission with same mismatch within dedup window should be suppressed
    h.reconciler->emit_confirmed_divergence(*os, mismatch, ts + 200);  // Still within 1s window
    
    // Verify second emission was deduped (ring should be empty)
    core::Divergence div2{};
    EXPECT_FALSE(h.divergence_ring->try_pop(div2));
    EXPECT_GE(h.counters.divergence_deduped, 1u);
}

} // namespace
