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

} // namespace
