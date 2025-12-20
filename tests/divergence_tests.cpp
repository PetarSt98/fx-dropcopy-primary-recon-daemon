#include <gtest/gtest.h>

#include "core/divergence.hpp"

namespace {

TEST(DivergenceTest, PhantomOrderDetection) {
    core::OrderState state{};
    state.key = 1;
    state.seen_dropcopy = true;
    state.dropcopy_status = core::OrdStatus::New;
    state.last_dropcopy_ts = 100;

    core::Divergence div{};
    ASSERT_TRUE(core::classify_divergence(state, div));
    EXPECT_EQ(div.type, core::DivergenceType::PhantomOrder);
    EXPECT_EQ(div.key, state.key);
}

TEST(DivergenceTest, MissingFillDetection) {
    core::OrderState state{};
    state.key = 2;
    state.seen_internal = true;
    state.seen_dropcopy = true;
    state.internal_status = core::OrdStatus::Working;
    state.internal_cum_qty = 10;
    state.internal_avg_px = 100;
    state.last_internal_ts = 200;
    state.dropcopy_status = core::OrdStatus::Filled;
    state.dropcopy_cum_qty = 20;
    state.dropcopy_avg_px = 105;
    state.last_dropcopy_ts = 150;

    core::Divergence div{};
    ASSERT_TRUE(core::classify_divergence(state, div));
    EXPECT_EQ(div.type, core::DivergenceType::MissingFill);
    EXPECT_EQ(div.internal_status, core::OrdStatus::Working);
    EXPECT_EQ(div.dropcopy_status, core::OrdStatus::Filled);
}

TEST(DivergenceTest, StateMismatchDetection) {
    core::OrderState state{};
    state.key = 3;
    state.seen_internal = true;
    state.seen_dropcopy = true;
    state.internal_status = core::OrdStatus::PartiallyFilled;
    state.dropcopy_status = core::OrdStatus::Filled;
    state.internal_cum_qty = 15;
    state.dropcopy_cum_qty = 15;

    core::Divergence div{};
    ASSERT_TRUE(core::classify_divergence(state, div));
    EXPECT_EQ(div.type, core::DivergenceType::StateMismatch);
    EXPECT_EQ(div.internal_status, core::OrdStatus::PartiallyFilled);
    EXPECT_EQ(div.dropcopy_status, core::OrdStatus::Filled);
}

TEST(DivergenceTest, QuantityMismatchDetection) {
    core::OrderState state{};
    state.key = 4;
    state.seen_internal = true;
    state.seen_dropcopy = true;
    state.internal_status = core::OrdStatus::Filled;
    state.dropcopy_status = core::OrdStatus::Filled;
    state.internal_cum_qty = 10;
    state.dropcopy_cum_qty = 20;
    state.internal_avg_px = 100;
    state.dropcopy_avg_px = 100;

    core::Divergence div{};
    ASSERT_TRUE(core::classify_divergence(state, div, 5));
    EXPECT_EQ(div.type, core::DivergenceType::QuantityMismatch);
    EXPECT_EQ(div.dropcopy_cum_qty, 20);
    EXPECT_EQ(div.internal_cum_qty, 10);
}

TEST(DivergenceTest, TimingAnomalyDetection) {
    core::OrderState state{};
    state.key = 5;
    state.seen_internal = true;
    state.seen_dropcopy = true;
    state.internal_status = core::OrdStatus::PartiallyFilled;
    state.dropcopy_status = core::OrdStatus::PartiallyFilled;
    state.internal_cum_qty = 10;
    state.dropcopy_cum_qty = 10;
    state.last_dropcopy_ts = 100;
    state.last_internal_ts = 200;

    core::Divergence div{};
    ASSERT_TRUE(core::classify_divergence(state, div, 0, 0, 50));
    EXPECT_EQ(div.type, core::DivergenceType::TimingAnomaly);
    EXPECT_EQ(div.dropcopy_ts, state.last_dropcopy_ts);
    EXPECT_EQ(div.internal_ts, state.last_internal_ts);
}

} // namespace
