#include <gtest/gtest.h>

#include "core/recon_config.hpp"

namespace {

// ReconConfig_DefaultValues - Verify default config values match expected
TEST(ReconConfigTest, DefaultValues) {
    const core::ReconConfig config{};

    EXPECT_EQ(config.grace_period_ns, 500'000'000u);  // 500ms
    EXPECT_EQ(config.gap_recheck_period_ns, 100'000'000u);  // 100ms
    EXPECT_EQ(config.divergence_dedup_window_ns, 1'000'000'000u);  // 1s

    EXPECT_EQ(config.qty_tolerance, 0);
    EXPECT_EQ(config.px_tolerance, 0);
    EXPECT_EQ(config.timing_slack_ns, 0u);

    EXPECT_TRUE(config.enable_windowed_recon);
    EXPECT_TRUE(config.enable_gap_suppression);
}

// ReconConfig_IsTriviallyCopyable - Static assert for trivially copyable
TEST(ReconConfigTest, IsTriviallyCopyable) {
    static_assert(std::is_trivially_copyable_v<core::ReconConfig>,
                  "ReconConfig must be trivially copyable");
}

// default_recon_config returns expected values
TEST(ReconConfigTest, DefaultReconConfigFunction) {
    constexpr auto config = core::default_recon_config();

    EXPECT_EQ(config.grace_period_ns, 500'000'000u);
    EXPECT_EQ(config.gap_recheck_period_ns, 100'000'000u);
    EXPECT_EQ(config.divergence_dedup_window_ns, 1'000'000'000u);
    EXPECT_TRUE(config.enable_windowed_recon);
    EXPECT_TRUE(config.enable_gap_suppression);
}

// ReconConfig_CustomValues - Verify custom config values
TEST(ReconConfigTest, CustomValues) {
    core::ReconConfig config{};
    config.grace_period_ns = 1'000'000'000;  // 1s
    config.gap_recheck_period_ns = 50'000'000;  // 50ms
    config.divergence_dedup_window_ns = 2'000'000'000;  // 2s
    config.qty_tolerance = 10;
    config.px_tolerance = 5;
    config.timing_slack_ns = 100'000;
    config.enable_windowed_recon = false;
    config.enable_gap_suppression = false;

    EXPECT_EQ(config.grace_period_ns, 1'000'000'000u);
    EXPECT_EQ(config.gap_recheck_period_ns, 50'000'000u);
    EXPECT_EQ(config.divergence_dedup_window_ns, 2'000'000'000u);
    EXPECT_EQ(config.qty_tolerance, 10);
    EXPECT_EQ(config.px_tolerance, 5);
    EXPECT_EQ(config.timing_slack_ns, 100'000u);
    EXPECT_FALSE(config.enable_windowed_recon);
    EXPECT_FALSE(config.enable_gap_suppression);
}

// ReconConfig_CopyConstruct - Verify copy constructor works
TEST(ReconConfigTest, CopyConstruct) {
    core::ReconConfig original{};
    original.grace_period_ns = 123456;
    original.qty_tolerance = 42;
    original.enable_windowed_recon = false;

    const core::ReconConfig copy = original;

    EXPECT_EQ(copy.grace_period_ns, original.grace_period_ns);
    EXPECT_EQ(copy.qty_tolerance, original.qty_tolerance);
    EXPECT_EQ(copy.enable_windowed_recon, original.enable_windowed_recon);
}

// ReconConfig_CopyAssign - Verify copy assignment works
TEST(ReconConfigTest, CopyAssign) {
    core::ReconConfig original{};
    original.grace_period_ns = 123456;
    original.qty_tolerance = 42;
    original.enable_windowed_recon = false;

    core::ReconConfig other{};
    other = original;

    EXPECT_EQ(other.grace_period_ns, original.grace_period_ns);
    EXPECT_EQ(other.qty_tolerance, original.qty_tolerance);
    EXPECT_EQ(other.enable_windowed_recon, original.enable_windowed_recon);
}

} // namespace
