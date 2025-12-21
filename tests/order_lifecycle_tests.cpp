#include <gtest/gtest.h>

#include "core/order_lifecycle.hpp"

namespace {

TEST(OrderLifecycleTest, ValidTransitionsApply) {
    core::OrdStatus status = core::OrdStatus::New;
    ASSERT_TRUE(core::apply_status_transition(status, core::OrdStatus::Working));
    EXPECT_EQ(status, core::OrdStatus::Working);

    ASSERT_TRUE(core::apply_status_transition(status, core::OrdStatus::PartiallyFilled));
    EXPECT_EQ(status, core::OrdStatus::PartiallyFilled);

    ASSERT_TRUE(core::apply_status_transition(status, core::OrdStatus::Filled));
    EXPECT_EQ(status, core::OrdStatus::Filled);
}

TEST(OrderLifecycleTest, InvalidTransitionsRejected) {
    core::OrdStatus status = core::OrdStatus::Filled;
    EXPECT_FALSE(core::apply_status_transition(status, core::OrdStatus::New));
    EXPECT_EQ(status, core::OrdStatus::Filled);

    status = core::OrdStatus::Canceled;
    EXPECT_FALSE(core::apply_status_transition(status, core::OrdStatus::Working));
    EXPECT_EQ(status, core::OrdStatus::Canceled);

    status = core::OrdStatus::PartiallyFilled;
    EXPECT_FALSE(core::apply_status_transition(status, core::OrdStatus::Working));
    EXPECT_EQ(status, core::OrdStatus::PartiallyFilled);
}

TEST(OrderLifecycleTest, UnknownAcceptsFirstStatus) {
    core::OrdStatus status = core::OrdStatus::Unknown;
    ASSERT_TRUE(core::is_valid_transition(status, core::OrdStatus::New));
    ASSERT_TRUE(core::apply_status_transition(status, core::OrdStatus::New));
    EXPECT_EQ(status, core::OrdStatus::New);
}

} // namespace
