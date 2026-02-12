#include <gtest/gtest.h>

#include <cstdint>
#include <thread>
#include <chrono>

#include "util/latency_histogram.hpp"
#include "util/perf_counters.hpp"
#include "util/perf_macros.hpp"
#include "util/rdtsc.hpp"
#include "util/tsc_calibration.hpp"

namespace {

// ============================================================================
// LatencyHistogram Tests
// ============================================================================

class LatencyHistogramTest : public ::testing::Test {
protected:
    // One-time TSC calibration for the entire test suite to avoid repeated 50ms sleeps
    static void SetUpTestSuite() {
        // Ensure TSC calibration is initialized for accurate time measurements
        util::TscCalibration::instance().calibrate_blocking(50);
    }
};

TEST_F(LatencyHistogramTest, BasicRecording) {
    util::LatencyHistogram<256, 10000> hist;
    
    EXPECT_EQ(hist.count(), 0);
    
    hist.record(1000);
    EXPECT_EQ(hist.count(), 1);
    EXPECT_EQ(hist.min(), 1000);
    EXPECT_EQ(hist.max(), 1000);
    EXPECT_EQ(hist.mean(), 1000);
}

TEST_F(LatencyHistogramTest, MinMaxMean) {
    util::LatencyHistogram<256, 10000> hist;
    
    hist.record(100);
    hist.record(200);
    hist.record(300);
    hist.record(400);
    hist.record(500);
    
    EXPECT_EQ(hist.count(), 5);
    EXPECT_EQ(hist.min(), 100);
    EXPECT_EQ(hist.max(), 500);
    EXPECT_EQ(hist.mean(), 300);  // (100+200+300+400+500)/5 = 300
}

TEST_F(LatencyHistogramTest, PercentileUniformDistribution) {
    util::LatencyHistogram<256, 10000> hist;
    
    // Record 1000 samples uniformly distributed from 0 to 9999 ns
    for (std::uint64_t i = 0; i < 1000; ++i) {
        hist.record(i * 10);  // 0, 10, 20, ..., 9990
    }
    
    // For uniform distribution, percentiles should be approximately:
    // P50 ~= 5000ns (middle of range)
    // P90 ~= 9000ns (90% of range)
    // P99 ~= 9900ns (99% of range)
    
    auto p50 = hist.percentile(0.50);
    auto p90 = hist.percentile(0.90);
    auto p99 = hist.percentile(0.99);
    
    // Allow ±10% tolerance due to bucketing
    EXPECT_NEAR(p50, 5000, 1000);
    EXPECT_NEAR(p90, 9000, 1000);
    EXPECT_NEAR(p99, 9900, 500);
}

TEST_F(LatencyHistogramTest, PercentileSkewedDistribution) {
    util::LatencyHistogram<256, 10000> hist;
    
    // Most samples in low range, few in high range
    for (int i = 0; i < 900; ++i) {
        hist.record(1000);  // 90% at 1000ns
    }
    for (int i = 0; i < 100; ++i) {
        hist.record(9000);  // 10% at 9000ns
    }
    
    auto p50 = hist.percentile(0.50);
    auto p90 = hist.percentile(0.90);
    auto p99 = hist.percentile(0.99);
    
    // P50 should be near 1000 (median is in the 90% cluster)
    EXPECT_LT(p50, 2000);
    // P90 should still be near 1000 (90th percentile is still in cluster)
    EXPECT_LT(p90, 2000);
    // P99 should be near 9000 (99th percentile hits the high cluster)
    EXPECT_GT(p99, 8000);
}

TEST_F(LatencyHistogramTest, OverflowBucket) {
    util::LatencyHistogram<256, 10000> hist;
    
    // Record values beyond max range
    hist.record(5000);
    hist.record(15000);  // Overflow
    hist.record(25000);  // Overflow
    
    EXPECT_EQ(hist.count(), 3);
    EXPECT_EQ(hist.min(), 5000);
    EXPECT_EQ(hist.max(), 25000);
    
    // P99.9 should return the max value from overflow bucket
    auto p999 = hist.percentile(0.999);
    EXPECT_EQ(p999, 25000);
}

TEST_F(LatencyHistogramTest, Reset) {
    util::LatencyHistogram<256, 10000> hist;
    
    hist.record(1000);
    hist.record(2000);
    hist.record(3000);
    
    EXPECT_EQ(hist.count(), 3);
    EXPECT_EQ(hist.min(), 1000);
    
    hist.reset();
    
    EXPECT_EQ(hist.count(), 0);
    EXPECT_EQ(hist.min(), 0);
    EXPECT_EQ(hist.max(), 0);
    EXPECT_EQ(hist.mean(), 0);
    EXPECT_EQ(hist.percentile(0.50), 0);
}

TEST_F(LatencyHistogramTest, PrintReport) {
    util::LatencyHistogram<256, 10000> hist;
    
    for (int i = 0; i < 100; ++i) {
        hist.record(1000 + i * 10);
    }
    
    // Just test that it doesn't crash
    testing::internal::CaptureStderr();
    hist.print_report("Test Operation");
    std::string output = testing::internal::GetCapturedStderr();
    
    EXPECT_TRUE(output.find("Test Operation") != std::string::npos);
    EXPECT_TRUE(output.find("P50") != std::string::npos);
    EXPECT_TRUE(output.find("P99") != std::string::npos);
}

// ============================================================================
// Performance Counter Tests (only when FX_PERF_ENABLED is defined)
// ============================================================================

#ifdef FX_PERF_ENABLED

TEST_F(LatencyHistogramTest, PerfCounterBasic) {
    auto& registry = util::PerfRegistry::instance();
    registry.reset();
    
    auto& counter = registry.get(util::PerfCounterId::HashTableLookup);
    
    EXPECT_EQ(counter.count, 0);
    
    counter.increment();
    EXPECT_EQ(counter.count, 1);
    
    counter.record_latency(1500);
    EXPECT_EQ(counter.count, 2);
    EXPECT_EQ(counter.latency_hist.count(), 1);
}

TEST_F(LatencyHistogramTest, PerfCounterLatencyTracking) {
    auto& registry = util::PerfRegistry::instance();
    registry.reset();
    
    auto& counter = registry.get(util::PerfCounterId::ArenaAllocate);
    
    counter.record_latency(1000);
    counter.record_latency(2000);
    counter.record_latency(3000);
    
    EXPECT_EQ(counter.count, 3);
    EXPECT_EQ(counter.latency_hist.count(), 3);
    EXPECT_EQ(counter.latency_hist.min(), 1000);
    EXPECT_EQ(counter.latency_hist.max(), 3000);
    EXPECT_EQ(counter.latency_hist.mean(), 2000);
}

TEST_F(LatencyHistogramTest, PerfCounterReset) {
    auto& registry = util::PerfRegistry::instance();
    registry.reset();
    
    auto& counter = registry.get(util::PerfCounterId::SpscRingPush);
    counter.record_latency(1000);
    counter.increment();
    
    EXPECT_GT(counter.count, 0);
    
    registry.reset();
    
    EXPECT_EQ(counter.count, 0);
    EXPECT_EQ(counter.latency_hist.count(), 0);
}

TEST_F(LatencyHistogramTest, PerfCounterDump) {
    auto& registry = util::PerfRegistry::instance();
    registry.reset();
    
    auto& counter = registry.get(util::PerfCounterId::ReconcilerProcessEvent);
    counter.record_latency(1500);
    counter.record_latency(2500);
    
    // Just test that dump doesn't crash
    testing::internal::CaptureStderr();
    registry.dump();
    std::string output = testing::internal::GetCapturedStderr();
    
    EXPECT_TRUE(output.find("Performance Counters") != std::string::npos);
}

#endif // FX_PERF_ENABLED

// ============================================================================
// Macro Tests
// ============================================================================

TEST_F(LatencyHistogramTest, MacrosCompile) {
    // Test that macros compile in both enabled and disabled modes
    PERF_SCOPE(util::PerfCounterId::HashTableLookup);
    
    PERF_START(test_timer);
    // Some work
    volatile int x = 0;
    for (int i = 0; i < 100; ++i) {
        x += i;
    }
    PERF_STOP(test_timer, util::PerfCounterId::HashTableUpsert);
    
    PERF_COUNT(util::PerfCounterId::AeronPoll);
    
    PERF_IF_ENABLED({
        // This code only runs when FX_PERF_ENABLED is defined
        volatile int y = 42;
        (void)y;
    });
    
    // If we reach here, all macros compiled successfully
    SUCCEED();
}

#ifdef FX_PERF_ENABLED

TEST_F(LatencyHistogramTest, PerfScopeMeasuresTime) {
    auto& registry = util::PerfRegistry::instance();
    registry.reset();
    
    {
        PERF_SCOPE(util::PerfCounterId::TimerWheelSchedule);
        
        // Do some work that takes measurable time
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
    
    auto& counter = registry.get(util::PerfCounterId::TimerWheelSchedule);
    EXPECT_EQ(counter.count, 1);
    EXPECT_GT(counter.latency_hist.count(), 0);
    // Should have measured at least a few microseconds
    EXPECT_GT(counter.latency_hist.mean(), 1000);  // At least 1us
}

TEST_F(LatencyHistogramTest, PerfStartStopMeasuresTime) {
    auto& registry = util::PerfRegistry::instance();
    registry.reset();
    
    PERF_START(my_operation);
    
    // Do some work
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    
    PERF_STOP(my_operation, util::PerfCounterId::TimerWheelPollExpired);
    
    auto& counter = registry.get(util::PerfCounterId::TimerWheelPollExpired);
    EXPECT_EQ(counter.count, 1);
    EXPECT_GT(counter.latency_hist.mean(), 1000);  // At least 1us
}

TEST_F(LatencyHistogramTest, PerfCountIncrementsOnly) {
    auto& registry = util::PerfRegistry::instance();
    registry.reset();
    
    PERF_COUNT(util::PerfCounterId::MismatchCompute);
    PERF_COUNT(util::PerfCounterId::MismatchCompute);
    PERF_COUNT(util::PerfCounterId::MismatchCompute);
    
    auto& counter = registry.get(util::PerfCounterId::MismatchCompute);
    EXPECT_EQ(counter.count, 3);
    EXPECT_EQ(counter.latency_hist.count(), 0);  // No latency recorded, just counts
}

#endif // FX_PERF_ENABLED

// ============================================================================
// Instrumentation Overhead Test
// ============================================================================

#ifdef FX_PERF_ENABLED

TEST_F(LatencyHistogramTest, InstrumentationOverhead) {
    auto& registry = util::PerfRegistry::instance();
    registry.reset();
    
    // Measure the overhead of PERF_SCOPE itself
    constexpr int iterations = 10000;
    
    const std::uint64_t start = util::rdtsc();
    
    for (int i = 0; i < iterations; ++i) {
        PERF_SCOPE(util::PerfCounterId::SpscRingPop);
        // Minimal work - just the instrumentation overhead
        volatile int x = i;
        (void)x;
    }
    
    const std::uint64_t end = util::rdtsc();
    const std::uint64_t total_ns = util::tsc_to_ns(end - start);
    const std::uint64_t avg_ns = total_ns / iterations;
    
    auto& counter = registry.get(util::PerfCounterId::SpscRingPop);
    EXPECT_EQ(counter.count, iterations);
    
    // Relaxed threshold for CI environments (virtualized, noisy neighbors, debug builds)
    // This test verifies instrumentation works, not strict performance guarantees
    // For production performance validation, use dedicated benchmark suite (FX-7061)
    EXPECT_LT(avg_ns, 10000) << "Average instrumentation overhead: " << avg_ns << " ns (sanity check)";
    
    // Verify that latency tracking is functioning
    EXPECT_GT(counter.latency_hist.count(), 0) << "Latency histogram should have recorded samples";
}

#endif // FX_PERF_ENABLED

} // namespace
