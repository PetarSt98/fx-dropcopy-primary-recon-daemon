#include <gtest/gtest.h>

#include "core/recon_config.hpp"
#include "core/divergence.hpp"
#include "tests/harness/scenario_builder.hpp"
#include "tests/harness/scenario_runner.hpp"

using namespace test;

// ===== Scenario A: DropCopy Leads, Primary Within Grace =====
// Expected: No divergence (primary arrives within grace period, false positive avoided)
TEST(ReconDeterminism, DropCopyLeadsPrimaryWithinGrace) {
    auto config = core::default_recon_config();
    config.grace_period_ns = 500'000'000;  // 500ms
    
    auto scenario = ReconScenarioBuilder()
        .starting_at(0)
        .dropcopy_fill("ORDER1", 100, to_micro(1.2345))
        .advance_time(std::chrono::milliseconds(50))
        .primary_fill("ORDER1", 100, to_micro(1.2345))
        .advance_time(std::chrono::milliseconds(500));
    
    auto result = run_scenario(scenario, config);
    
    EXPECT_EQ(result.confirmed_divergences.size(), 0);
    EXPECT_EQ(result.counters.false_positive_avoided, 1);
    EXPECT_EQ(result.counters.orders_matched, 1);
}

// ===== Scenario B: Primary Missing Beyond Grace =====
// Expected: Confirmed divergence (phantom order, primary never arrives)
TEST(ReconDeterminism, PrimaryMissingBeyondGrace) {
    auto config = core::default_recon_config();
    config.grace_period_ns = 200'000'000;  // 200ms
    
    auto scenario = ReconScenarioBuilder()
        .starting_at(0)
        .dropcopy_fill("ORDER1", 100, to_micro(1.2345))
        .advance_time(std::chrono::milliseconds(600));
    
    auto result = run_scenario(scenario, config);
    
    ASSERT_EQ(result.confirmed_divergences.size(), 1);
    EXPECT_EQ(result.confirmed_divergences[0].type, core::DivergenceType::PhantomOrder);
    EXPECT_EQ(result.counters.mismatch_confirmed, 1);
}

// ===== Scenario C: Gap Suppresses Confirmation =====
// Expected: No confirmed divergence (suppressed due to sequence gap)
TEST(ReconDeterminism, GapSuppressesConfirmation) {
    auto config = core::default_recon_config();
    config.grace_period_ns = 200'000'000;
    config.enable_gap_suppression = true;
    
    auto scenario = ReconScenarioBuilder()
        .starting_at(0)
        // Create sequence gap (seq 1, then jump to 4)
        .primary_working("ORDER1")  // seq 1
        .sequence_gap(core::Source::Primary, 2, 3)
        .advance_time(std::chrono::milliseconds(10))
        .primary_working("ORDER2")  // seq 4 (gap detected)
        .advance_time(std::chrono::milliseconds(10))
        .dropcopy_fill("ORDER1", 100, to_micro(1.2345))
        .advance_time(std::chrono::milliseconds(600));
    
    auto result = run_scenario(scenario, config);
    
    // The divergence should be suppressed due to gap
    EXPECT_EQ(result.confirmed_divergences.size(), 0);
    EXPECT_GT(result.counters.gap_suppressions, 0);
}

// ===== Scenario D: Replay Produces Identical Output =====
// Expected: Running the same scenario twice produces identical results
TEST(ReconDeterminism, ReplayProducesIdenticalOutput) {
    auto config = core::default_recon_config();
    
    // Complex scenario with multiple orders
    auto scenario = ReconScenarioBuilder()
        .starting_at(0)
        .primary_new_order("ORDER1")
        .advance_time_ms(10)
        .dropcopy_new_order("ORDER1")
        .advance_time_ms(20)
        .primary_partial_fill("ORDER1", 50, to_micro(1.2345))
        .advance_time_ms(5)
        .dropcopy_fill("ORDER1", 100, to_micro(1.2340))  // Mismatch
        .advance_time_ms(500);
    
    // Run twice
    auto result1 = run_scenario(scenario, config);
    auto result2 = run_scenario(scenario, config);
    
    // Must be identical
    ASSERT_EQ(result1.confirmed_divergences.size(), result2.confirmed_divergences.size());
    for (size_t i = 0; i < result1.confirmed_divergences.size(); ++i) {
        EXPECT_EQ(result1.confirmed_divergences[i].key, result2.confirmed_divergences[i].key);
        EXPECT_EQ(result1.confirmed_divergences[i].type, result2.confirmed_divergences[i].type);
    }
    
    EXPECT_EQ(result1.counters.mismatch_confirmed, result2.counters.mismatch_confirmed);
    EXPECT_EQ(result1.counters.false_positive_avoided, result2.counters.false_positive_avoided);
}

// ===== Additional Test: Verify Determinism Helper Function =====
TEST(ReconDeterminism, CheckDeterminismHelperWorks) {
    auto scenario = ReconScenarioBuilder()
        .starting_at(0)
        .primary_fill("ORDER1", 100, to_micro(1.2345))
        .advance_time_ms(5)
        .dropcopy_fill("ORDER1", 100, to_micro(1.2345))
        .advance_time_ms(10);
    
    // Should be deterministic
    EXPECT_TRUE(check_determinism(scenario));
}

// ===== Additional Test: Quantity Mismatch =====
TEST(ReconDeterminism, QuantityMismatchDetected) {
    auto config = core::default_recon_config();
    config.grace_period_ns = 100'000'000;  // 100ms
    
    auto scenario = ReconScenarioBuilder()
        .starting_at(0)
        .primary_fill("ORDER1", 100, to_micro(1.2345))
        .advance_time(std::chrono::milliseconds(10))
        .dropcopy_fill("ORDER1", 150, to_micro(1.2345))  // Different quantity
        .advance_time(std::chrono::milliseconds(200));
    
    auto result = run_scenario(scenario, config);
    
    ASSERT_EQ(result.confirmed_divergences.size(), 1);
    EXPECT_EQ(result.confirmed_divergences[0].type, core::DivergenceType::QuantityMismatch);
    EXPECT_EQ(result.counters.mismatch_confirmed, 1);
}

// ===== Additional Test: Price Mismatch =====
TEST(ReconDeterminism, PriceMismatchDetected) {
    auto config = core::default_recon_config();
    config.grace_period_ns = 100'000'000;  // 100ms
    
    auto scenario = ReconScenarioBuilder()
        .starting_at(0)
        .primary_fill("ORDER1", 100, to_micro(1.2345))
        .advance_time(std::chrono::milliseconds(10))
        .dropcopy_fill("ORDER1", 100, to_micro(1.5000))  // Different price
        .advance_time(std::chrono::milliseconds(200));
    
    auto result = run_scenario(scenario, config);
    
    ASSERT_EQ(result.confirmed_divergences.size(), 1);
    // Price mismatch is classified as StateMismatch
    EXPECT_EQ(result.confirmed_divergences[0].type, core::DivergenceType::StateMismatch);
    EXPECT_EQ(result.counters.mismatch_confirmed, 1);
}

// ===== Additional Test: Multiple Orders =====
TEST(ReconDeterminism, MultipleOrdersHandledCorrectly) {
    auto config = core::default_recon_config();
    config.grace_period_ns = 200'000'000;  // 200ms
    
    auto scenario = ReconScenarioBuilder()
        .starting_at(0)
        // Order 1: Matches perfectly
        .primary_fill("ORDER1", 100, to_micro(1.2345))
        .advance_time_ms(5)
        .dropcopy_fill("ORDER1", 100, to_micro(1.2345))
        // Order 2: Diverges (missing primary)
        .advance_time_ms(5)
        .dropcopy_fill("ORDER2", 200, to_micro(2.5000))
        // Order 3: Matches after brief delay
        .advance_time_ms(5)
        .dropcopy_fill("ORDER3", 300, to_micro(3.7500))
        .advance_time_ms(50)
        .primary_fill("ORDER3", 300, to_micro(3.7500))
        .advance_time(std::chrono::milliseconds(500));
    
    auto result = run_scenario(scenario, config);
    
    // ORDER2 should diverge (phantom), ORDER1 and ORDER3 should match
    ASSERT_EQ(result.confirmed_divergences.size(), 1);
    EXPECT_EQ(result.confirmed_divergences[0].type, core::DivergenceType::PhantomOrder);
    EXPECT_EQ(result.counters.orders_matched, 2);  // ORDER1 and ORDER3
    EXPECT_GE(result.counters.false_positive_avoided, 1);  // At least ORDER3
}
