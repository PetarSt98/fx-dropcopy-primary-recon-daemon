#include <cstdint>

#include <gtest/gtest.h>

#include "util/arena.hpp"

namespace {

class ArenaTest : public ::testing::Test {
protected:
    util::Arena arena_{64};
};

TEST_F(ArenaTest, BasicAllocation) {
    void* p1 = arena_.allocate(16, alignof(std::uint64_t));
    void* p2 = arena_.allocate(16, alignof(std::uint32_t));
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_NE(p1, p2);
}

TEST_F(ArenaTest, Alignment) {
    constexpr std::size_t alignment = 32;
    void* p = arena_.allocate(8, alignment);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignment, 0u);
}

TEST_F(ArenaTest, Exhaustion) {
    void* p1 = arena_.allocate(16, alignof(std::uint64_t));
    void* p2 = arena_.allocate(16, alignof(std::uint64_t));
    void* p3 = arena_.allocate(8, alignof(std::uint64_t));

    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    EXPECT_EQ(p3, nullptr);
}

TEST_F(ArenaTest, ResetReuse) {
    void* first = arena_.allocate(24, alignof(std::uint64_t));
    void* exhausted = arena_.allocate(16, alignof(std::uint64_t));
    arena_.reset();
    void* after_reset = arena_.allocate(24, alignof(std::uint64_t));

    ASSERT_NE(first, nullptr);
    EXPECT_EQ(exhausted, nullptr);
    ASSERT_NE(after_reset, nullptr);
    EXPECT_EQ(after_reset, first);
}

} // namespace
