#include <gtest/gtest.h>

#include "util/fixed_vec.hpp"

namespace {

// Trivially copyable struct for testing (not just primitives)
struct TimerEntry {
    std::uint64_t deadline{0};
    std::uint32_t id{0};
    std::uint8_t priority{0};
};

static_assert(std::is_trivially_copyable_v<TimerEntry>, "TimerEntry must be trivially copyable");

using IntVec = util::FixedCapacityVec<int, 8>;
using TimerVec = util::FixedCapacityVec<TimerEntry, 4>;

class FixedCapacityVecTest : public ::testing::Test {
protected:
    IntVec int_vec_{};
    TimerVec timer_vec_{};
};

TEST_F(FixedCapacityVecTest, DefaultConstructionIsEmpty) {
    EXPECT_TRUE(int_vec_.empty());
    EXPECT_EQ(int_vec_.size(), 0u);
    EXPECT_FALSE(int_vec_.full());
}

TEST_F(FixedCapacityVecTest, CapacityAndMaxSize) {
    EXPECT_EQ(IntVec::capacity(), 8u);
    EXPECT_EQ(IntVec::max_size(), 8u);
    EXPECT_EQ(TimerVec::capacity(), 4u);
    EXPECT_EQ(TimerVec::max_size(), 4u);
}

TEST_F(FixedCapacityVecTest, TryPushBackSuccess) {
    EXPECT_TRUE(int_vec_.try_push_back(42));
    EXPECT_EQ(int_vec_.size(), 1u);
    EXPECT_FALSE(int_vec_.empty());
    EXPECT_EQ(int_vec_[0], 42);
}

TEST_F(FixedCapacityVecTest, TryPushBackReturnsFalseWhenFull) {
    // Fill the vector
    for (int i = 0; i < 8; ++i) {
        ASSERT_TRUE(int_vec_.try_push_back(i)) << "Failed to push at index " << i;
    }
    EXPECT_TRUE(int_vec_.full());
    EXPECT_EQ(int_vec_.size(), 8u);

    // Should return false, not crash or throw
    EXPECT_FALSE(int_vec_.try_push_back(999));
    EXPECT_EQ(int_vec_.size(), 8u);  // Size unchanged
}

TEST_F(FixedCapacityVecTest, TryEmplaceBackSuccess) {
    EXPECT_TRUE(timer_vec_.try_emplace_back(TimerEntry{100, 1, 5}));
    EXPECT_EQ(timer_vec_.size(), 1u);
    EXPECT_EQ(timer_vec_[0].deadline, 100u);
    EXPECT_EQ(timer_vec_[0].id, 1u);
    EXPECT_EQ(timer_vec_[0].priority, 5u);
}

TEST_F(FixedCapacityVecTest, TryEmplaceBackReturnsFalseWhenFull) {
    // Fill the vector
    for (std::uint32_t i = 0; i < 4; ++i) {
        ASSERT_TRUE(timer_vec_.try_emplace_back(TimerEntry{i * 100, i, 0})) << "Failed to emplace at index " << i;
    }
    EXPECT_TRUE(timer_vec_.full());

    // Should return false, not crash or throw
    EXPECT_FALSE(timer_vec_.try_emplace_back(TimerEntry{999, 999, 255}));
    EXPECT_EQ(timer_vec_.size(), 4u);
}

TEST_F(FixedCapacityVecTest, PopBack) {
    int_vec_.try_push_back(10);
    int_vec_.try_push_back(20);
    EXPECT_EQ(int_vec_.size(), 2u);

    int_vec_.pop_back();
    EXPECT_EQ(int_vec_.size(), 1u);
    EXPECT_EQ(int_vec_[0], 10);

    int_vec_.pop_back();
    EXPECT_TRUE(int_vec_.empty());
}

TEST_F(FixedCapacityVecTest, ClearResetsSize) {
    int_vec_.try_push_back(1);
    int_vec_.try_push_back(2);
    int_vec_.try_push_back(3);
    EXPECT_EQ(int_vec_.size(), 3u);

    int_vec_.clear();
    EXPECT_TRUE(int_vec_.empty());
    EXPECT_EQ(int_vec_.size(), 0u);
    EXPECT_FALSE(int_vec_.full());
}

TEST_F(FixedCapacityVecTest, ClearAllowsReuse) {
    // Fill completely
    for (int i = 0; i < 8; ++i) {
        int_vec_.try_push_back(i);
    }
    EXPECT_TRUE(int_vec_.full());

    // Clear and reuse
    int_vec_.clear();
    EXPECT_TRUE(int_vec_.try_push_back(100));
    EXPECT_EQ(int_vec_.size(), 1u);
    EXPECT_EQ(int_vec_[0], 100);
}

TEST_F(FixedCapacityVecTest, FrontAndBack) {
    int_vec_.try_push_back(10);
    int_vec_.try_push_back(20);
    int_vec_.try_push_back(30);

    EXPECT_EQ(int_vec_.front(), 10);
    EXPECT_EQ(int_vec_.back(), 30);

    // Modify through front/back
    int_vec_.front() = 11;
    int_vec_.back() = 33;
    EXPECT_EQ(int_vec_[0], 11);
    EXPECT_EQ(int_vec_[2], 33);
}

TEST_F(FixedCapacityVecTest, DataPointer) {
    int_vec_.try_push_back(1);
    int_vec_.try_push_back(2);
    int_vec_.try_push_back(3);

    int* ptr = int_vec_.data();
    ASSERT_NE(ptr, nullptr);
    EXPECT_EQ(ptr[0], 1);
    EXPECT_EQ(ptr[1], 2);
    EXPECT_EQ(ptr[2], 3);
}

TEST_F(FixedCapacityVecTest, RangeBasedForLoop) {
    int_vec_.try_push_back(10);
    int_vec_.try_push_back(20);
    int_vec_.try_push_back(30);

    int sum = 0;
    for (const auto& val : int_vec_) {
        sum += val;
    }
    EXPECT_EQ(sum, 60);
}

TEST_F(FixedCapacityVecTest, IteratorCorrectness) {
    int_vec_.try_push_back(5);
    int_vec_.try_push_back(10);
    int_vec_.try_push_back(15);

    EXPECT_EQ(int_vec_.end() - int_vec_.begin(), 3);
    EXPECT_EQ(*int_vec_.begin(), 5);
    EXPECT_EQ(*(int_vec_.end() - 1), 15);
    EXPECT_EQ(int_vec_.cbegin(), int_vec_.begin());
    EXPECT_EQ(int_vec_.cend(), int_vec_.end());
}

TEST_F(FixedCapacityVecTest, SwapEraseMiddle) {
    int_vec_.try_push_back(1);
    int_vec_.try_push_back(2);
    int_vec_.try_push_back(3);
    int_vec_.try_push_back(4);

    // Remove element at index 1 (value 2)
    // Last element (4) should replace it
    int_vec_.swap_erase(1);

    EXPECT_EQ(int_vec_.size(), 3u);
    EXPECT_EQ(int_vec_[0], 1);
    EXPECT_EQ(int_vec_[1], 4);  // Was the last element
    EXPECT_EQ(int_vec_[2], 3);
}

TEST_F(FixedCapacityVecTest, SwapEraseLast) {
    int_vec_.try_push_back(1);
    int_vec_.try_push_back(2);
    int_vec_.try_push_back(3);

    // Remove last element (no swap needed)
    int_vec_.swap_erase(2);

    EXPECT_EQ(int_vec_.size(), 2u);
    EXPECT_EQ(int_vec_[0], 1);
    EXPECT_EQ(int_vec_[1], 2);
}

TEST_F(FixedCapacityVecTest, SwapEraseFirst) {
    int_vec_.try_push_back(1);
    int_vec_.try_push_back(2);
    int_vec_.try_push_back(3);

    // Remove first element
    int_vec_.swap_erase(0);

    EXPECT_EQ(int_vec_.size(), 2u);
    EXPECT_EQ(int_vec_[0], 3);  // Was the last element
    EXPECT_EQ(int_vec_[1], 2);
}

TEST_F(FixedCapacityVecTest, SwapEraseSingleElement) {
    int_vec_.try_push_back(42);

    int_vec_.swap_erase(0);

    EXPECT_TRUE(int_vec_.empty());
}

TEST_F(FixedCapacityVecTest, EmptyAndFullBoundaries) {
    // Initially empty, not full
    EXPECT_TRUE(int_vec_.empty());
    EXPECT_FALSE(int_vec_.full());

    // Add one element: not empty, not full
    int_vec_.try_push_back(1);
    EXPECT_FALSE(int_vec_.empty());
    EXPECT_FALSE(int_vec_.full());

    // Fill to capacity-1: not empty, not full
    for (int i = 2; i <= 7; ++i) {
        int_vec_.try_push_back(i);
    }
    EXPECT_FALSE(int_vec_.empty());
    EXPECT_FALSE(int_vec_.full());
    EXPECT_EQ(int_vec_.size(), 7u);

    // Fill to capacity: not empty, full
    int_vec_.try_push_back(8);
    EXPECT_FALSE(int_vec_.empty());
    EXPECT_TRUE(int_vec_.full());
    EXPECT_EQ(int_vec_.size(), 8u);
}

TEST_F(FixedCapacityVecTest, WorksWithTriviallyCopyableStruct) {
    TimerEntry t1{1000, 1, 10};
    TimerEntry t2{2000, 2, 20};

    EXPECT_TRUE(timer_vec_.try_push_back(t1));
    EXPECT_TRUE(timer_vec_.try_push_back(t2));

    EXPECT_EQ(timer_vec_.size(), 2u);
    EXPECT_EQ(timer_vec_[0].deadline, 1000u);
    EXPECT_EQ(timer_vec_[1].id, 2u);

    // Range-based for with struct
    std::uint64_t total_deadline = 0;
    for (const auto& entry : timer_vec_) {
        total_deadline += entry.deadline;
    }
    EXPECT_EQ(total_deadline, 3000u);
}

TEST_F(FixedCapacityVecTest, IndexOperatorReadWrite) {
    int_vec_.try_push_back(100);
    EXPECT_EQ(int_vec_[0], 100);

    int_vec_[0] = 200;
    EXPECT_EQ(int_vec_[0], 200);
}

TEST_F(FixedCapacityVecTest, ConstAccess) {
    int_vec_.try_push_back(1);
    int_vec_.try_push_back(2);
    int_vec_.try_push_back(3);

    const IntVec& const_ref = int_vec_;

    EXPECT_EQ(const_ref.size(), 3u);
    EXPECT_EQ(const_ref[0], 1);
    EXPECT_EQ(const_ref.front(), 1);
    EXPECT_EQ(const_ref.back(), 3);
    EXPECT_NE(const_ref.data(), nullptr);
    EXPECT_FALSE(const_ref.empty());
    EXPECT_FALSE(const_ref.full());

    // Const iteration
    int sum = 0;
    for (auto it = const_ref.cbegin(); it != const_ref.cend(); ++it) {
        sum += *it;
    }
    EXPECT_EQ(sum, 6);
}

} // namespace
