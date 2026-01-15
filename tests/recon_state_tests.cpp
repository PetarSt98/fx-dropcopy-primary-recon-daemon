#include <gtest/gtest.h>

#include "core/recon_state.hpp"

namespace {

// ============================================================================
// MismatchMask Tests
// ============================================================================

TEST(MismatchMaskTest, SizeIsOneByte) {
    EXPECT_EQ(sizeof(core::MismatchMask), 1u);
}

TEST(MismatchMaskTest, IsTriviallyCopyable) {
    EXPECT_TRUE(std::is_trivially_copyable_v<core::MismatchMask>);
}

TEST(MismatchMaskTest, IsStandardLayout) {
    EXPECT_TRUE(std::is_standard_layout_v<core::MismatchMask>);
}

TEST(MismatchMaskTest, DefaultConstructedIsNone) {
    core::MismatchMask mask{};
    EXPECT_TRUE(mask.none());
    EXPECT_FALSE(mask.any());
    EXPECT_EQ(mask.bits(), 0u);
}

TEST(MismatchMaskTest, AnyReturnsTrueAfterSettingFlag) {
    core::MismatchMask mask{};
    mask.set(core::MismatchMask::STATUS);
    EXPECT_TRUE(mask.any());
    EXPECT_FALSE(mask.none());
}

TEST(MismatchMaskTest, BitsMatchesExpectedAfterSettingFlags) {
    core::MismatchMask mask{};
    mask.set(core::MismatchMask::STATUS);
    mask.set(core::MismatchMask::CUM_QTY);
    
    const std::uint8_t expected = core::MismatchMask::STATUS | core::MismatchMask::CUM_QTY;
    EXPECT_EQ(mask.bits(), expected);
}

TEST(MismatchMaskTest, SetHasAndClear) {
    core::MismatchMask mask{};
    
    // Set a flag
    mask.set(core::MismatchMask::AVG_PX);
    EXPECT_TRUE(mask.has(core::MismatchMask::AVG_PX));
    EXPECT_FALSE(mask.has(core::MismatchMask::STATUS));
    
    // Set another flag
    mask.set(core::MismatchMask::EXISTENCE);
    EXPECT_TRUE(mask.has(core::MismatchMask::AVG_PX));
    EXPECT_TRUE(mask.has(core::MismatchMask::EXISTENCE));
    
    // Clear a flag
    mask.clear(core::MismatchMask::AVG_PX);
    EXPECT_FALSE(mask.has(core::MismatchMask::AVG_PX));
    EXPECT_TRUE(mask.has(core::MismatchMask::EXISTENCE));
}

TEST(MismatchMaskTest, AllFlagsWork) {
    core::MismatchMask mask{};
    
    // Test each flag individually
    mask.set(core::MismatchMask::STATUS);
    EXPECT_TRUE(mask.has(core::MismatchMask::STATUS));
    mask.clear(core::MismatchMask::STATUS);
    
    mask.set(core::MismatchMask::CUM_QTY);
    EXPECT_TRUE(mask.has(core::MismatchMask::CUM_QTY));
    mask.clear(core::MismatchMask::CUM_QTY);
    
    mask.set(core::MismatchMask::LEAVES_QTY);
    EXPECT_TRUE(mask.has(core::MismatchMask::LEAVES_QTY));
    mask.clear(core::MismatchMask::LEAVES_QTY);
    
    mask.set(core::MismatchMask::AVG_PX);
    EXPECT_TRUE(mask.has(core::MismatchMask::AVG_PX));
    mask.clear(core::MismatchMask::AVG_PX);
    
    mask.set(core::MismatchMask::EXISTENCE);
    EXPECT_TRUE(mask.has(core::MismatchMask::EXISTENCE));
    mask.clear(core::MismatchMask::EXISTENCE);
    
    mask.set(core::MismatchMask::EXEC_ID);
    EXPECT_TRUE(mask.has(core::MismatchMask::EXEC_ID));
    mask.clear(core::MismatchMask::EXEC_ID);
    
    EXPECT_TRUE(mask.none());
}

TEST(MismatchMaskTest, EqualityOperator) {
    core::MismatchMask mask1{};
    core::MismatchMask mask2{};
    
    EXPECT_TRUE(mask1 == mask2);
    EXPECT_FALSE(mask1 != mask2);
    
    mask1.set(core::MismatchMask::STATUS);
    EXPECT_FALSE(mask1 == mask2);
    EXPECT_TRUE(mask1 != mask2);
    
    mask2.set(core::MismatchMask::STATUS);
    EXPECT_TRUE(mask1 == mask2);
    EXPECT_FALSE(mask1 != mask2);
}

TEST(MismatchMaskTest, InequalityOperator) {
    core::MismatchMask mask1{};
    core::MismatchMask mask2{};
    
    mask1.set(core::MismatchMask::CUM_QTY);
    mask2.set(core::MismatchMask::AVG_PX);
    
    EXPECT_TRUE(mask1 != mask2);
    EXPECT_FALSE(mask1 == mask2);
}

// ============================================================================
// ReconState Tests
// ============================================================================

TEST(ReconStateTest, ToStringReturnsNonNullForAllStates) {
    EXPECT_NE(core::to_string(core::ReconState::Unknown), nullptr);
    EXPECT_NE(core::to_string(core::ReconState::AwaitingPrimary), nullptr);
    EXPECT_NE(core::to_string(core::ReconState::AwaitingDropCopy), nullptr);
    EXPECT_NE(core::to_string(core::ReconState::InGrace), nullptr);
    EXPECT_NE(core::to_string(core::ReconState::Matched), nullptr);
    EXPECT_NE(core::to_string(core::ReconState::DivergedConfirmed), nullptr);
    EXPECT_NE(core::to_string(core::ReconState::SuppressedByGap), nullptr);
}

TEST(ReconStateTest, ToStringReturnsExpectedValues) {
    EXPECT_STREQ(core::to_string(core::ReconState::Unknown), "Unknown");
    EXPECT_STREQ(core::to_string(core::ReconState::AwaitingPrimary), "AwaitingPrimary");
    EXPECT_STREQ(core::to_string(core::ReconState::AwaitingDropCopy), "AwaitingDropCopy");
    EXPECT_STREQ(core::to_string(core::ReconState::InGrace), "InGrace");
    EXPECT_STREQ(core::to_string(core::ReconState::Matched), "Matched");
    EXPECT_STREQ(core::to_string(core::ReconState::DivergedConfirmed), "DivergedConfirmed");
    EXPECT_STREQ(core::to_string(core::ReconState::SuppressedByGap), "SuppressedByGap");
}

TEST(ReconStateTest, ToStringHandlesInvalidValue) {
    // Cast an out-of-range value
    const auto invalid_state = static_cast<core::ReconState>(99);
    const char* result = core::to_string(invalid_state);
    EXPECT_NE(result, nullptr);
    EXPECT_STREQ(result, "Unknown");
}

TEST(ReconStateTest, IsTerminalReconStateReturnsTrueForTerminalStates) {
    EXPECT_TRUE(core::is_terminal_recon_state(core::ReconState::Matched));
    EXPECT_TRUE(core::is_terminal_recon_state(core::ReconState::DivergedConfirmed));
}

TEST(ReconStateTest, IsTerminalReconStateReturnsFalseForNonTerminalStates) {
    EXPECT_FALSE(core::is_terminal_recon_state(core::ReconState::Unknown));
    EXPECT_FALSE(core::is_terminal_recon_state(core::ReconState::AwaitingPrimary));
    EXPECT_FALSE(core::is_terminal_recon_state(core::ReconState::AwaitingDropCopy));
    EXPECT_FALSE(core::is_terminal_recon_state(core::ReconState::InGrace));
    EXPECT_FALSE(core::is_terminal_recon_state(core::ReconState::SuppressedByGap));
}

} // namespace
