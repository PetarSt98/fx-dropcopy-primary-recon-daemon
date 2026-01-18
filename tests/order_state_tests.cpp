#include <gtest/gtest.h>

#include "core/order_state.hpp"

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

} // namespace
