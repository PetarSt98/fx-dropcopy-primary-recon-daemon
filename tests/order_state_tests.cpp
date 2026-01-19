#include <gtest/gtest.h>

#include "core/order_state.hpp"
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
// FX-7054: Gap Uncertainty Flags Tests
// ============================================================================

TEST(OrderStateTest, GapUncertaintyFlagsSetClear) {
    // Test mark/clear functions for gap uncertainty flags
    core::OrderState os{};
    core::SequenceTracker trk{};
    
    // Initialize tracker and create a gap
    core::init_sequence_tracker(trk, 1);
    core::SequenceGapEvent evt{};
    core::track_sequence(trk, core::Source::Primary, 0, 5, 1000, &evt);  // Create gap
    ASSERT_TRUE(trk.gap_open);
    
    // Initially no flags set
    EXPECT_EQ(os.gap_uncertainty_flags, 0u);
    
    // Mark primary gap uncertainty
    core::mark_gap_uncertainty(os, core::Source::Primary, trk);
    EXPECT_EQ(os.gap_uncertainty_flags, core::GapUncertaintyFlags::PRIMARY);
    EXPECT_EQ(os.gap_suppression_epoch, trk.gap_epoch);
    
    // Mark dropcopy gap uncertainty
    core::mark_gap_uncertainty(os, core::Source::DropCopy, trk);
    EXPECT_EQ(os.gap_uncertainty_flags, core::GapUncertaintyFlags::PRIMARY | core::GapUncertaintyFlags::DROPCOPY);
    
    // Clear primary gap uncertainty
    core::clear_gap_uncertainty(os, core::Source::Primary);
    EXPECT_EQ(os.gap_uncertainty_flags, core::GapUncertaintyFlags::DROPCOPY);
    
    // Clear dropcopy gap uncertainty
    core::clear_gap_uncertainty(os, core::Source::DropCopy);
    EXPECT_EQ(os.gap_uncertainty_flags, 0u);
}

TEST(OrderStateTest, HasGapUncertaintyDetection) {
    // Test has_gap_uncertainty() function
    core::OrderState os{};
    
    // Initially no gap uncertainty
    EXPECT_FALSE(core::has_gap_uncertainty(os));
    
    // Set primary flag
    os.gap_uncertainty_flags = core::GapUncertaintyFlags::PRIMARY;
    EXPECT_TRUE(core::has_gap_uncertainty(os));
    
    // Set dropcopy flag
    os.gap_uncertainty_flags = core::GapUncertaintyFlags::DROPCOPY;
    EXPECT_TRUE(core::has_gap_uncertainty(os));
    
    // Set both flags
    os.gap_uncertainty_flags = core::GapUncertaintyFlags::PRIMARY | core::GapUncertaintyFlags::DROPCOPY;
    EXPECT_TRUE(core::has_gap_uncertainty(os));
    
    // Clear all
    os.gap_uncertainty_flags = 0;
    EXPECT_FALSE(core::has_gap_uncertainty(os));
}

TEST(OrderStateTest, GapFlagsPerSourceIndependent) {
    // Test that Primary and DropCopy flags are independent
    core::OrderState os{};
    core::SequenceTracker trk_primary{};
    core::SequenceTracker trk_dropcopy{};
    
    // Initialize both trackers
    core::init_sequence_tracker(trk_primary, 1);
    core::init_sequence_tracker(trk_dropcopy, 1);
    
    // Create gaps in both trackers
    core::SequenceGapEvent evt{};
    core::track_sequence(trk_primary, core::Source::Primary, 0, 5, 1000, &evt);
    core::track_sequence(trk_dropcopy, core::Source::DropCopy, 1, 5, 1000, &evt);
    
    ASSERT_TRUE(trk_primary.gap_open);
    ASSERT_TRUE(trk_dropcopy.gap_open);
    
    // Mark only primary
    core::mark_gap_uncertainty(os, core::Source::Primary, trk_primary);
    EXPECT_TRUE(os.gap_uncertainty_flags & core::GapUncertaintyFlags::PRIMARY);
    EXPECT_FALSE(os.gap_uncertainty_flags & core::GapUncertaintyFlags::DROPCOPY);
    
    // Mark dropcopy - primary should still be set
    core::mark_gap_uncertainty(os, core::Source::DropCopy, trk_dropcopy);
    EXPECT_TRUE(os.gap_uncertainty_flags & core::GapUncertaintyFlags::PRIMARY);
    EXPECT_TRUE(os.gap_uncertainty_flags & core::GapUncertaintyFlags::DROPCOPY);
    
    // Clear primary - dropcopy should still be set
    core::clear_gap_uncertainty(os, core::Source::Primary);
    EXPECT_FALSE(os.gap_uncertainty_flags & core::GapUncertaintyFlags::PRIMARY);
    EXPECT_TRUE(os.gap_uncertainty_flags & core::GapUncertaintyFlags::DROPCOPY);
    
    // Clear dropcopy - both should be unset
    core::clear_gap_uncertainty(os, core::Source::DropCopy);
    EXPECT_FALSE(os.gap_uncertainty_flags & core::GapUncertaintyFlags::PRIMARY);
    EXPECT_FALSE(os.gap_uncertainty_flags & core::GapUncertaintyFlags::DROPCOPY);
    EXPECT_EQ(os.gap_uncertainty_flags, 0u);
}

TEST(OrderStateTest, ClearAllGapUncertainty) {
    // Test clear_all_gap_uncertainty() function
    core::OrderState os{};
    
    // Set both flags and epoch
    os.gap_uncertainty_flags = core::GapUncertaintyFlags::PRIMARY | core::GapUncertaintyFlags::DROPCOPY;
    os.gap_suppression_epoch = 5;
    
    // Clear all
    core::clear_all_gap_uncertainty(os);
    
    EXPECT_EQ(os.gap_uncertainty_flags, 0u);
    EXPECT_EQ(os.gap_suppression_epoch, 0u);
}

TEST(OrderStateTest, MarkGapUncertaintyNoOpWhenGapClosed) {
    // Test that mark_gap_uncertainty does nothing when gap is closed
    core::OrderState os{};
    core::SequenceTracker trk{};
    
    // Initialize tracker but no gap
    core::init_sequence_tracker(trk, 1);
    ASSERT_FALSE(trk.gap_open);
    
    // Try to mark gap uncertainty - should be no-op
    core::mark_gap_uncertainty(os, core::Source::Primary, trk);
    EXPECT_EQ(os.gap_uncertainty_flags, 0u);
    EXPECT_EQ(os.gap_suppression_epoch, 0u);
}

TEST(OrderStateTest, GapUncertaintyFlagsInitialization) {
    // Verify gap_uncertainty_flags is initialized to 0
    util::Arena arena(1024);
    core::OrderState* state = core::create_order_state(arena, 42);
    ASSERT_NE(state, nullptr);
    
    EXPECT_EQ(state->gap_uncertainty_flags, 0u);
}

TEST(OrderStateTest, MarkGapUncertainty_MultipleSources_TracksMaxEpoch) {
    OrderState os{};
    
    SequenceTracker primary_tracker{};
    primary_tracker.gap_open = true;
    primary_tracker.gap_epoch = 5;
    
    SequenceTracker dropcopy_tracker{};
    dropcopy_tracker. gap_open = true;
    dropcopy_tracker.gap_epoch = 7;
    
    // Mark Primary first (lower epoch)
    mark_gap_uncertainty(os, Source::Primary, primary_tracker);
    EXPECT_EQ(os.gap_suppression_epoch, 5);
    EXPECT_EQ(os.gap_uncertainty_flags, GapUncertaintyFlags:: PRIMARY);
    
    // Mark DropCopy - should update to max epoch, not overwrite
    mark_gap_uncertainty(os, Source:: DropCopy, dropcopy_tracker);
    EXPECT_EQ(os.gap_suppression_epoch, 7);  // Should be MAX
    EXPECT_EQ(os.gap_uncertainty_flags, 
              GapUncertaintyFlags::PRIMARY | GapUncertaintyFlags::DROPCOPY);
    
    // Test reverse order:  DropCopy marked first (higher epoch)
    OrderState os2{};
    mark_gap_uncertainty(os2, Source::DropCopy, dropcopy_tracker);
    mark_gap_uncertainty(os2, Source::Primary, primary_tracker);
    EXPECT_EQ(os2.gap_suppression_epoch, 7);  // Still MAX
}

} // namespace
