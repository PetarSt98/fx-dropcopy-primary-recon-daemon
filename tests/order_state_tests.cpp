#include <gtest/gtest.h>

#include "core/order_state.hpp"
#include "core/gap_uncertainty.hpp"
#include "util/tsc_calibration.hpp"

namespace {

TEST(OrderStateTest, OrderKeyStability) {
    core::ExecEvent evt{};
    evt.set_clord_id("ABC123", 6);
    const core::OrderKey key1 = core::make_order_key(evt);
    const core::OrderKey key2 = core::make_order_key(evt);

    core::ExecEvent other{};
    other.set_clord_id("XYZ789", 6);
    const core::OrderKey key3 = core::make_order_key(other);

    EXPECT_EQ(key1, key2);
    EXPECT_NE(key1, key3);
}

TEST(OrderStateTest, CreateOrderStateInitialization) {
    util::Arena arena(1024);
    constexpr core::OrderKey expected_key = 42;
    core::OrderState* state = core::create_order_state(arena, expected_key);
    ASSERT_NE(state, nullptr);

    EXPECT_EQ(state->key, expected_key);
    EXPECT_EQ(state->internal_cum_qty, 0);
    EXPECT_EQ(state->internal_avg_px, 0);
    EXPECT_EQ(state->dropcopy_cum_qty, 0);
    EXPECT_EQ(state->dropcopy_avg_px, 0);
    EXPECT_EQ(state->internal_status, core::OrdStatus::Unknown);
    EXPECT_EQ(state->dropcopy_status, core::OrdStatus::Unknown);
    EXPECT_EQ(state->last_internal_exec_id_len, 0);
    EXPECT_EQ(state->last_dropcopy_exec_id_len, 0);
    EXPECT_FALSE(state->seen_internal);
    EXPECT_FALSE(state->seen_dropcopy);
    EXPECT_FALSE(state->has_divergence);
    EXPECT_FALSE(state->has_gap);
    EXPECT_EQ(state->divergence_count, 0);
    EXPECT_EQ(state->last_internal_exec_id[0], '\0');
    EXPECT_EQ(state->last_dropcopy_exec_id[0], '\0');
}

// ============================================================================
// FX-7051 Part 2: Reconciliation Overlay Tests
// ============================================================================

TEST(OrderStateTest, DefaultInit_ReconFieldsZeroed) {
    util::Arena arena(1024);
    core::OrderState* state = core::create_order_state(arena, 42);
    ASSERT_NE(state, nullptr);

    // Verify recon overlay fields are properly initialized
    EXPECT_EQ(state->recon_state, core::ReconState::Unknown);
    EXPECT_EQ(state->primary_last_seen_tsc, 0u);
    EXPECT_EQ(state->dropcopy_last_seen_tsc, 0u);
    EXPECT_EQ(state->mismatch_first_seen_tsc, 0u);
    EXPECT_EQ(state->recon_deadline_tsc, 0u);
    EXPECT_EQ(state->timer_generation, 0u);
    EXPECT_EQ(state->gap_suppression_epoch, 0u);
    EXPECT_TRUE(state->current_mismatch.none());
}

TEST(ComputeMismatchTest, NoneSeen_ReturnsEmpty) {
    core::OrderState os{};
    os.seen_internal = false;
    os.seen_dropcopy = false;

    auto mask = core::compute_mismatch(os);
    EXPECT_TRUE(mask.none());
}

TEST(ComputeMismatchTest, ExistenceMismatch_PrimaryOnly) {
    core::OrderState os{};
    os.seen_internal = true;
    os.seen_dropcopy = false;

    auto mask = core::compute_mismatch(os);
    EXPECT_TRUE(mask.has(core::MismatchMask::EXISTENCE));
    // Should ONLY have EXISTENCE set (early return)
    EXPECT_FALSE(mask.has(core::MismatchMask::STATUS));
    EXPECT_FALSE(mask.has(core::MismatchMask::CUM_QTY));
}

TEST(ComputeMismatchTest, ExistenceMismatch_DropCopyOnly) {
    core::OrderState os{};
    os.seen_internal = false;
    os.seen_dropcopy = true;

    auto mask = core::compute_mismatch(os);
    EXPECT_TRUE(mask.has(core::MismatchMask::EXISTENCE));
    // Should ONLY have EXISTENCE set (early return)
    EXPECT_FALSE(mask.has(core::MismatchMask::STATUS));
}

TEST(ComputeMismatchTest, StatusMismatch) {
    core::OrderState os{};
    os.seen_internal = true;
    os.seen_dropcopy = true;
    os.internal_status = core::OrdStatus::Working;
    os.dropcopy_status = core::OrdStatus::Filled;

    auto mask = core::compute_mismatch(os);
    EXPECT_TRUE(mask.has(core::MismatchMask::STATUS));
}

TEST(ComputeMismatchTest, CumQtyMismatch) {
    core::OrderState os{};
    os.seen_internal = true;
    os.seen_dropcopy = true;
    os.internal_status = core::OrdStatus::PartiallyFilled;
    os.dropcopy_status = core::OrdStatus::PartiallyFilled;
    os.internal_cum_qty = 100;
    os.dropcopy_cum_qty = 200;

    auto mask = core::compute_mismatch(os);
    EXPECT_TRUE(mask.has(core::MismatchMask::CUM_QTY));
}

TEST(ComputeMismatchTest, AvgPxMismatch) {
    core::OrderState os{};
    os.seen_internal = true;
    os.seen_dropcopy = true;
    os.internal_status = core::OrdStatus::Filled;
    os.dropcopy_status = core::OrdStatus::Filled;
    os.internal_avg_px = 1000000;  // micro-units
    os.dropcopy_avg_px = 1000500;  // micro-units

    auto mask = core::compute_mismatch(os);
    EXPECT_TRUE(mask.has(core::MismatchMask::AVG_PX));
}

TEST(ComputeMismatchTest, ExecIdMismatch_LenDiffers) {
    core::OrderState os{};
    os.seen_internal = true;
    os.seen_dropcopy = true;
    os.internal_status = core::OrdStatus::New;
    os.dropcopy_status = core::OrdStatus::New;
    std::memcpy(os.last_internal_exec_id, "EXEC1", 5);
    os.last_internal_exec_id_len = 5;
    std::memcpy(os.last_dropcopy_exec_id, "EXEC123", 7);
    os.last_dropcopy_exec_id_len = 7;

    auto mask = core::compute_mismatch(os);
    EXPECT_TRUE(mask.has(core::MismatchMask::EXEC_ID));
}

TEST(ComputeMismatchTest, ExecIdMismatch_ContentDiffers) {
    core::OrderState os{};
    os.seen_internal = true;
    os.seen_dropcopy = true;
    os.internal_status = core::OrdStatus::New;
    os.dropcopy_status = core::OrdStatus::New;
    std::memcpy(os.last_internal_exec_id, "EXEC1", 5);
    os.last_internal_exec_id_len = 5;
    std::memcpy(os.last_dropcopy_exec_id, "EXEC2", 5);
    os.last_dropcopy_exec_id_len = 5;

    auto mask = core::compute_mismatch(os);
    EXPECT_TRUE(mask.has(core::MismatchMask::EXEC_ID));
}

TEST(ComputeMismatchTest, ExecIdMatches_NoMismatch) {
    core::OrderState os{};
    os.seen_internal = true;
    os.seen_dropcopy = true;
    os.internal_status = core::OrdStatus::New;
    os.dropcopy_status = core::OrdStatus::New;
    std::memcpy(os.last_internal_exec_id, "SAME1", 5);
    os.last_internal_exec_id_len = 5;
    std::memcpy(os.last_dropcopy_exec_id, "SAME1", 5);
    os.last_dropcopy_exec_id_len = 5;

    auto mask = core::compute_mismatch(os);
    EXPECT_FALSE(mask.has(core::MismatchMask::EXEC_ID));
}

TEST(ComputeMismatchTest, MultipleBits) {
    core::OrderState os{};
    os.seen_internal = true;
    os.seen_dropcopy = true;
    // Status mismatch
    os.internal_status = core::OrdStatus::Working;
    os.dropcopy_status = core::OrdStatus::Filled;
    // CumQty mismatch
    os.internal_cum_qty = 100;
    os.dropcopy_cum_qty = 200;
    // AvgPx mismatch
    os.internal_avg_px = 1000;
    os.dropcopy_avg_px = 2000;

    auto mask = core::compute_mismatch(os);
    EXPECT_TRUE(mask.has(core::MismatchMask::STATUS));
    EXPECT_TRUE(mask.has(core::MismatchMask::CUM_QTY));
    EXPECT_TRUE(mask.has(core::MismatchMask::AVG_PX));
}

TEST(ComputeMismatchTest, LeavesQtyNotComputed_V1) {
    // LEAVES_QTY is not computed in v1 because order_qty is not tracked.
    // Verify it remains unset regardless of other mismatches.
    core::OrderState os{};
    os.seen_internal = true;
    os.seen_dropcopy = true;
    os.internal_status = core::OrdStatus::PartiallyFilled;
    os.dropcopy_status = core::OrdStatus::PartiallyFilled;
    os.internal_cum_qty = 50;
    os.dropcopy_cum_qty = 50;

    auto mask = core::compute_mismatch(os);
    // LEAVES_QTY should never be set in v1
    EXPECT_FALSE(mask.has(core::MismatchMask::LEAVES_QTY));
}

TEST(ComputeMismatchTest, BothSeenAndMatch_ReturnsEmpty) {
    core::OrderState os{};
    os.seen_internal = true;
    os.seen_dropcopy = true;
    os.internal_status = core::OrdStatus::Filled;
    os.dropcopy_status = core::OrdStatus::Filled;
    os.internal_cum_qty = 100;
    os.dropcopy_cum_qty = 100;
    os.internal_avg_px = 1000;
    os.dropcopy_avg_px = 1000;
    // Empty exec IDs
    os.last_internal_exec_id_len = 0;
    os.last_dropcopy_exec_id_len = 0;

    auto mask = core::compute_mismatch(os);
    EXPECT_TRUE(mask.none());
}

// ============================================================================
// FX-7053 Part 1: Divergence Deduplication Tests
// ============================================================================

TEST(OrderStateTest, DefaultInit_DivergenceFieldsZeroed) {
    util::Arena arena(1024);
    core::OrderState* state = core::create_order_state(arena, 42);
    ASSERT_NE(state, nullptr);

    // Verify divergence tracking fields are properly initialized
    EXPECT_EQ(state->last_divergence_emit_tsc, 0u);
    EXPECT_TRUE(state->last_emitted_mismatch.none());
    EXPECT_EQ(state->divergence_emit_count, 0u);
}

TEST(OrderStateTest, SizeWithinLimit) {
    // Explicit test for size constraint (also checked by static_assert)
    EXPECT_LE(sizeof(core::OrderState), 256u);
}

TEST(ShouldEmitDivergenceTest, NeverEmitted_ReturnsTrue) {
    core::OrderState os{};
    core::MismatchMask mismatch{};
    mismatch.set(core::MismatchMask::STATUS);

    // First emission should always return true (last_divergence_emit_tsc == 0)
    EXPECT_TRUE(core::should_emit_divergence(os, mismatch, 1000, 1'000'000'000));
}

TEST(ShouldEmitDivergenceTest, SameMismatchWithinWindow_ReturnsFalse) {
    core::OrderState os{};
    core::MismatchMask mismatch{};
    mismatch.set(core::MismatchMask::STATUS);

    // Use TSC-consistent values
    // dedup_window_ns = 1 second = 1'000'000'000 ns
    // dedup_window_tsc = ns_to_tsc(1s) = 3 billion cycles (at 3GHz)
    const std::uint64_t dedup_window_ns = 1'000'000'000;
    const std::uint64_t dedup_window_tsc = util::ns_to_tsc(dedup_window_ns);
    
    // Simulate a previous emission
    os.last_divergence_emit_tsc = 1000;
    os.last_emitted_mismatch = mismatch;

    // Same mismatch within dedup window (500 cycles << 3 billion cycles)
    EXPECT_FALSE(core::should_emit_divergence(os, mismatch, 1500, dedup_window_ns));
}

TEST(ShouldEmitDivergenceTest, SameMismatchAfterWindow_ReturnsTrue) {
    core::OrderState os{};
    core::MismatchMask mismatch{};
    mismatch.set(core::MismatchMask::STATUS);

    // Use TSC-consistent values
    const std::uint64_t dedup_window_ns = 1'000'000'000;
    const std::uint64_t dedup_window_tsc = util::ns_to_tsc(dedup_window_ns);
    
    // Simulate a previous emission
    os.last_divergence_emit_tsc = 1000;
    os.last_emitted_mismatch = mismatch;

    // Same mismatch after window expires (TSC elapsed >= converted window)
    EXPECT_TRUE(core::should_emit_divergence(os, mismatch, 1000 + dedup_window_tsc, dedup_window_ns));
}

TEST(ShouldEmitDivergenceTest, DifferentMismatch_ReturnsTrue) {
    core::OrderState os{};
    core::MismatchMask old_mismatch{};
    old_mismatch.set(core::MismatchMask::STATUS);

    core::MismatchMask new_mismatch{};
    new_mismatch.set(core::MismatchMask::CUM_QTY);

    // Simulate a previous emission with different mismatch
    os.last_divergence_emit_tsc = 1000;
    os.last_emitted_mismatch = old_mismatch;

    // Different mismatch should always return true (even within window)
    EXPECT_TRUE(core::should_emit_divergence(os, new_mismatch, 1001, 1'000'000'000));
}

TEST(ShouldEmitDivergenceTest, ClockAnomaly_ReturnsTrue) {
    core::OrderState os{};
    core::MismatchMask mismatch{};
    mismatch.set(core::MismatchMask::STATUS);

    // Simulate a previous emission at a future timestamp (clock anomaly/rollover)
    os.last_divergence_emit_tsc = 10000;
    os.last_emitted_mismatch = mismatch;

    // When now_tsc < last_divergence_emit_tsc, should emit to be safe
    EXPECT_TRUE(core::should_emit_divergence(os, mismatch, 5000, 1'000'000'000));
}

TEST(RecordDivergenceEmissionTest, UpdatesAllFields) {
    core::OrderState os{};
    core::MismatchMask mismatch{};
    mismatch.set(core::MismatchMask::STATUS);
    mismatch.set(core::MismatchMask::CUM_QTY);

    // Record emission
    core::record_divergence_emission(os, mismatch, 12345);

    EXPECT_EQ(os.last_divergence_emit_tsc, 12345u);
    EXPECT_EQ(os.last_emitted_mismatch, mismatch);
    EXPECT_EQ(os.divergence_emit_count, 1u);

    // Record another emission
    core::MismatchMask mismatch2{};
    mismatch2.set(core::MismatchMask::AVG_PX);
    core::record_divergence_emission(os, mismatch2, 67890);

    EXPECT_EQ(os.last_divergence_emit_tsc, 67890u);
    EXPECT_EQ(os.last_emitted_mismatch, mismatch2);
    EXPECT_EQ(os.divergence_emit_count, 2u);
}

// ============================================================================
// FX-7054: Gap Uncertainty Flag Tests
// ============================================================================

TEST(OrderStateGapUncertaintyTest, MarkGapUncertainty_SetsFlag) {
    core::OrderState os{};
    core::SequenceTracker trk{};
    core::init_sequence_tracker(trk, 1);

    // Create a gap to make tracker.gap_open true
    core::SequenceGapEvent evt{};
    core::track_sequence(trk, core::Source::Primary, 0, 5, 1000, &evt);

    EXPECT_TRUE(trk.gap_open);
    EXPECT_EQ(os.gap_uncertainty_flags, 0u);

    // Mark for Primary source
    core::mark_gap_uncertainty(os, core::Source::Primary, trk);
    EXPECT_TRUE(core::has_gap_uncertainty_for(os, core::Source::Primary));
    EXPECT_FALSE(core::has_gap_uncertainty_for(os, core::Source::DropCopy));
}

TEST(OrderStateGapUncertaintyTest, MarkGapUncertainty_IncrementsTrackerCount) {
    core::OrderState os{};
    core::SequenceTracker trk{};
    core::init_sequence_tracker(trk, 1);

    // Create a gap
    core::SequenceGapEvent evt{};
    core::track_sequence(trk, core::Source::Primary, 0, 5, 1000, &evt);

    EXPECT_EQ(trk.orders_in_gap_count, 0u);

    // Mark first time
    core::mark_gap_uncertainty(os, core::Source::Primary, trk);
    EXPECT_EQ(trk.orders_in_gap_count, 1u);

    // Mark same source again - should NOT increment
    core::mark_gap_uncertainty(os, core::Source::Primary, trk);
    EXPECT_EQ(trk.orders_in_gap_count, 1u);
}

TEST(OrderStateGapUncertaintyTest, ClearGapUncertainty_ClearsFlag) {
    core::OrderState os{};
    core::SequenceTracker trk{};
    core::init_sequence_tracker(trk, 1);

    // Create a gap and mark
    core::SequenceGapEvent evt{};
    core::track_sequence(trk, core::Source::Primary, 0, 5, 1000, &evt);
    core::mark_gap_uncertainty(os, core::Source::Primary, trk);

    EXPECT_TRUE(core::has_gap_uncertainty_for(os, core::Source::Primary));

    // Clear the flag - should return true (was marked)
    EXPECT_TRUE(core::clear_gap_uncertainty(os, core::Source::Primary, nullptr));
    EXPECT_FALSE(core::has_gap_uncertainty_for(os, core::Source::Primary));

    // Clear again - should return false (was already clear)
    EXPECT_FALSE(core::clear_gap_uncertainty(os, core::Source::Primary, nullptr));
}

TEST(OrderStateGapUncertaintyTest, ClearGapUncertainty_DecrementsTrackerCount) {
    core::OrderState os{};
    core::SequenceTracker trk{};
    core::init_sequence_tracker(trk, 1);

    // Create a gap and mark
    core::SequenceGapEvent evt{};
    core::track_sequence(trk, core::Source::Primary, 0, 5, 1000, &evt);
    core::mark_gap_uncertainty(os, core::Source::Primary, trk);

    EXPECT_EQ(trk.orders_in_gap_count, 1u);

    // Clear with tracker reference - count should decrement
    EXPECT_TRUE(core::clear_gap_uncertainty(os, core::Source::Primary, trk));
    EXPECT_EQ(trk.orders_in_gap_count, 0u);

    // Clear again - should return false and count stays at 0
    EXPECT_FALSE(core::clear_gap_uncertainty(os, core::Source::Primary, &trk));
    EXPECT_EQ(trk.orders_in_gap_count, 0u);
}

TEST(OrderStateGapUncertaintyTest, GapFlags_PerSourceIndependent) {
    core::OrderState os{};
    core::SequenceTracker primary_trk{};
    core::SequenceTracker dropcopy_trk{};
    core::init_sequence_tracker(primary_trk, 1);
    core::init_sequence_tracker(dropcopy_trk, 1);

    // Create gaps on both trackers
    core::SequenceGapEvent evt{};
    core::track_sequence(primary_trk, core::Source::Primary, 0, 5, 1000, &evt);
    core::track_sequence(dropcopy_trk, core::Source::DropCopy, 0, 5, 1000, &evt);

    // Mark both sources
    core::mark_gap_uncertainty(os, core::Source::Primary, primary_trk);
    core::mark_gap_uncertainty(os, core::Source::DropCopy, dropcopy_trk);

    EXPECT_TRUE(core::has_gap_uncertainty_for(os, core::Source::Primary));
    EXPECT_TRUE(core::has_gap_uncertainty_for(os, core::Source::DropCopy));
    EXPECT_TRUE(core::has_gap_uncertainty(os));

    // Clear Primary only - use reference overload
    EXPECT_TRUE(core::clear_gap_uncertainty(os, core::Source::Primary, primary_trk));
    EXPECT_FALSE(core::has_gap_uncertainty_for(os, core::Source::Primary));
    EXPECT_TRUE(core::has_gap_uncertainty_for(os, core::Source::DropCopy));
    EXPECT_TRUE(core::has_gap_uncertainty(os));  // Still has DropCopy flag

    // Clear DropCopy - use pointer overload
    EXPECT_TRUE(core::clear_gap_uncertainty(os, core::Source::DropCopy, &dropcopy_trk));
    EXPECT_FALSE(core::has_gap_uncertainty_for(os, core::Source::Primary));
    EXPECT_FALSE(core::has_gap_uncertainty_for(os, core::Source::DropCopy));
    EXPECT_FALSE(core::has_gap_uncertainty(os));  // No flags set
}

TEST(OrderStateGapUncertaintyTest, HasGapUncertainty_DetectsAnyFlag) {
    core::OrderState os{};
    core::SequenceTracker trk{};
    core::init_sequence_tracker(trk, 1);

    // Create a gap
    core::SequenceGapEvent evt{};
    core::track_sequence(trk, core::Source::Primary, 0, 5, 1000, &evt);

    EXPECT_FALSE(core::has_gap_uncertainty(os));

    // Set Primary flag
    core::mark_gap_uncertainty(os, core::Source::Primary, trk);
    EXPECT_TRUE(core::has_gap_uncertainty(os));

    // Clear and set DropCopy flag
    (void)core::clear_gap_uncertainty(os, core::Source::Primary, nullptr);
    core::mark_gap_uncertainty(os, core::Source::DropCopy, trk);
    EXPECT_TRUE(core::has_gap_uncertainty(os));
}

TEST(OrderStateGapUncertaintyTest, HasGapUncertaintyFor_DetectsSpecificSource) {
    core::OrderState os{};
    core::SequenceTracker trk{};
    core::init_sequence_tracker(trk, 1);

    // Create a gap
    core::SequenceGapEvent evt{};
    core::track_sequence(trk, core::Source::Primary, 0, 5, 1000, &evt);

    // Mark DropCopy only
    core::mark_gap_uncertainty(os, core::Source::DropCopy, trk);

    EXPECT_FALSE(core::has_gap_uncertainty_for(os, core::Source::Primary));
    EXPECT_TRUE(core::has_gap_uncertainty_for(os, core::Source::DropCopy));
}

TEST(OrderStateGapUncertaintyTest, ClearAllGapUncertainty_ClearsAllFlags) {
    core::OrderState os{};
    core::SequenceTracker trk{};
    core::init_sequence_tracker(trk, 1);

    // Create a gap
    core::SequenceGapEvent evt{};
    core::track_sequence(trk, core::Source::Primary, 0, 5, 1000, &evt);

    // Mark both sources
    core::mark_gap_uncertainty(os, core::Source::Primary, trk);
    core::mark_gap_uncertainty(os, core::Source::DropCopy, trk);

    EXPECT_TRUE(core::has_gap_uncertainty(os));

    // Clear all
    core::clear_all_gap_uncertainty(os);

    EXPECT_FALSE(core::has_gap_uncertainty(os));
    EXPECT_FALSE(core::has_gap_uncertainty_for(os, core::Source::Primary));
    EXPECT_FALSE(core::has_gap_uncertainty_for(os, core::Source::DropCopy));
}

TEST(OrderStateGapUncertaintyTest, MarkGapUncertainty_NoOpWhenGapNotOpen) {
    core::OrderState os{};
    core::SequenceTracker trk{};
    core::init_sequence_tracker(trk, 1);

    // No gap - tracker.gap_open is false
    EXPECT_FALSE(trk.gap_open);

    // Try to mark - should be no-op
    core::mark_gap_uncertainty(os, core::Source::Primary, trk);

    EXPECT_FALSE(core::has_gap_uncertainty(os));
    EXPECT_EQ(trk.orders_in_gap_count, 0u);
}

TEST(OrderStateGapUncertaintyTest, MarkGapUncertainty_UpdatesEpoch) {
    core::OrderState os{};
    core::SequenceTracker trk{};
    core::init_sequence_tracker(trk, 1);

    // Create a gap
    core::SequenceGapEvent evt{};
    core::track_sequence(trk, core::Source::Primary, 0, 5, 1000, &evt);

    EXPECT_EQ(trk.gap_epoch, 1u);
    EXPECT_EQ(os.gap_suppression_epoch, 0u);

    // Mark - should update epoch
    core::mark_gap_uncertainty(os, core::Source::Primary, trk);
    EXPECT_EQ(os.gap_suppression_epoch, 1u);
}

TEST(OrderStateGapUncertaintyTest, GapUncertaintyFlags_DefaultInitialized) {
    util::Arena arena(1024);
    core::OrderState* state = core::create_order_state(arena, 42);
    ASSERT_NE(state, nullptr);

    EXPECT_EQ(state->gap_uncertainty_flags, 0u);
    EXPECT_FALSE(core::has_gap_uncertainty(*state));
}

} // namespace
