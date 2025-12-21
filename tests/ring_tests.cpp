#include <gtest/gtest.h>

#include "ingest/spsc_ring.hpp"
#include "core/exec_event.hpp"

using Ring = ingest::SpscRing<core::ExecEvent, 8>;

namespace {

class SpscRingTest : public ::testing::Test {
protected:
    Ring ring_{};
    core::ExecEvent evt_{};
};

TEST_F(SpscRingTest, PushPopOrder) {
    core::ExecEvent first{};
    first.qty = 1;
    core::ExecEvent second{};
    second.qty = 2;
    core::ExecEvent out{};

    ASSERT_TRUE(ring_.try_push(first));
    ASSERT_TRUE(ring_.try_push(second));

    ASSERT_TRUE(ring_.try_pop(out));
    EXPECT_EQ(out.qty, 1);

    ASSERT_TRUE(ring_.try_pop(out));
    EXPECT_EQ(out.qty, 2);
}

TEST_F(SpscRingTest, FullEmptyBehavior) {
    for (int i = 0; i < 7; ++i) {
        ASSERT_TRUE(ring_.try_push(evt_)) << "Failed to push at index " << i;
    }

    EXPECT_FALSE(ring_.try_push(evt_)) << "Push should fail when the ring is full";

    for (int i = 0; i < 7; ++i) {
        ASSERT_TRUE(ring_.try_pop(evt_)) << "Failed to pop at index " << i;
    }

    EXPECT_FALSE(ring_.try_pop(evt_)) << "Pop should fail when the ring is empty";
}

} // namespace
