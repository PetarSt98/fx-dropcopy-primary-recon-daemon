#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <chrono>

#include "util/latency_histogram.hpp"
#include "util/perf_counters.hpp"
#include "util/perf_macros.hpp"
#include "util/rdtsc.hpp"
#include "util/tsc_calibration.hpp"

#ifdef FX_PERF_ENABLED
#include "core/divergence.hpp"
#include "core/exec_event.hpp"
#include "core/order_state.hpp"
#include "core/order_state_store.hpp"
#include "core/reconciler.hpp"
#include "core/recon_config.hpp"
#include "ingest/spsc_ring.hpp"
#include "util/arena.hpp"
#include "util/wheel_timer.hpp"
#endif

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

TEST_F(LatencyHistogramTest, PercentileEdgeCases) {
    util::LatencyHistogram<256, 10000> hist;
    
    // Test with several samples
    hist.record(100);   // min
    hist.record(500);
    hist.record(1000);
    hist.record(5000);  // max
    
    // Test p=0.0 returns min
    EXPECT_EQ(hist.percentile(0.0), hist.min());
    EXPECT_EQ(hist.percentile(0.0), 100);
    
    // Test p=1.0 returns max
    EXPECT_EQ(hist.percentile(1.0), hist.max());
    EXPECT_EQ(hist.percentile(1.0), 5000);
    
    // Test clamping for out-of-range p values
    EXPECT_EQ(hist.percentile(-0.5), hist.min());  // Should clamp to 0.0
    EXPECT_EQ(hist.percentile(1.5), hist.max());   // Should clamp to 1.0
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
    PERF_SCOPE(::util::PerfCounterId::HashTableLookup);
    
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
        PERF_SCOPE(::util::PerfCounterId::TimerWheelSchedule);
        
        // Do some work that takes measurable time
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
    
    auto& counter = registry.get(util::PerfCounterId::TimerWheelSchedule);
    // Functional assertions (not strict timing)
    EXPECT_EQ(counter.count, 1);
    EXPECT_GT(counter.latency_hist.count(), 0);
    EXPECT_GT(counter.latency_hist.sum(), 0);  // Should have recorded some time
    EXPECT_GT(counter.latency_hist.max(), 0);  // Max should be non-zero
}

TEST_F(LatencyHistogramTest, PerfStartStopMeasuresTime) {
    auto& registry = util::PerfRegistry::instance();
    registry.reset();
    
    PERF_START(my_operation);
    
    // Do some work
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    
    PERF_STOP(my_operation, util::PerfCounterId::TimerWheelPollExpired);
    
    auto& counter = registry.get(util::PerfCounterId::TimerWheelPollExpired);
    // Functional assertions (not strict timing)
    EXPECT_EQ(counter.count, 1);
    EXPECT_GT(counter.latency_hist.count(), 0);
    EXPECT_GT(counter.latency_hist.sum(), 0);  // Should have recorded some time
    EXPECT_GT(counter.latency_hist.max(), 0);  // Max should be non-zero
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
        PERF_SCOPE(::util::PerfCounterId::SpscRingPop);
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
    EXPECT_GT(counter.latency_hist.count(), 0)
        << "Latency histogram should have recorded samples";

    registry.dump();
}

// ============================================================================
// Comprehensive Performance Report
// Exercises every instrumented component and dumps all counters with
// Min/Mean/Max/P50/P99/P99.9 histograms.
//
// Run:  ./unit_tests_gtest --gtest_filter="LatencyHistogramTest.FullPerfReport"
// ============================================================================

TEST_F(LatencyHistogramTest, FullPerfReport) {
    auto& registry = util::PerfRegistry::instance();
    registry.reset();

    constexpr int ITERATIONS = 50000;

    // ---- 1. Arena allocation (PerfCounterId::ArenaAllocate) ----
    {
        util::Arena arena(2ULL * 1024ULL * 1024ULL);  // 2 MB
        for (int i = 0; i < ITERATIONS; ++i) {
            void* ptr = arena.allocate(sizeof(core::OrderState),
                                       alignof(core::OrderState));
            (void)ptr;
            // Reset periodically to avoid exhaustion
            if ((i & 0xFFF) == 0xFFF) {
                arena.reset();
            }
        }
    }

    // ---- 2. Hash table upsert + lookup (PerfCounterId::HashTableUpsert,
    //         PerfCounterId::HashTableLookup) ----
    {
        util::Arena arena(4ULL * 1024ULL * 1024ULL);  // 4 MB
        core::OrderStateStore store(arena, 2048);

        for (int i = 0; i < ITERATIONS; ++i) {
            core::ExecEvent ev{};
            ev.ord_status = core::OrdStatus::New;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "ORD%05d", i % 1000);
            std::memcpy(ev.clord_id, buf, 8);
            ev.clord_id_len = 8;
            ev.cum_qty = 0;
            ev.price_micro = 1000000;

            auto* os = store.upsert(ev);
            (void)os;

            // Also exercise lookup
            const core::OrderKey key = core::make_order_key(ev);
            auto* found = store.find(key);
            (void)found;

            // Reset periodically to avoid overflow
            if (i % 1000 == 999) {
                store.reset_epoch();
            }
        }
    }

    // ---- 3. Timer wheel schedule + poll (PerfCounterId::TimerWheelSchedule,
    //         PerfCounterId::TimerWheelPollExpired) ----
    {
        util::WheelTimer timer(0);
        const std::uint64_t tick_tsc = timer.tick_tsc();

        for (int i = 0; i < ITERATIONS; ++i) {
            std::uint64_t deadline = tick_tsc +
                static_cast<std::uint64_t>(i % 4096) * tick_tsc;
            timer.schedule(static_cast<core::OrderKey>(i), 0, deadline);

            // Poll periodically to exercise poll_expired
            if ((i & 0xFF) == 0xFF) {
                timer.poll_expired(
                    tick_tsc + static_cast<std::uint64_t>(i) * tick_tsc,
                    [](core::OrderKey, std::uint32_t) {});
            }
        }
        // Final poll to flush remaining entries
        timer.poll_expired(
            tick_tsc * (ITERATIONS + 4096),
            [](core::OrderKey, std::uint32_t) {});
    }

    // ---- 4. SPSC ring push + pop (PerfCounterId::SpscRingPush,
    //         PerfCounterId::SpscRingPop) ----
    {
        ingest::SpscRing<core::ExecEvent, 1u << 16> ring;
        core::ExecEvent evt{};
        evt.ord_status = core::OrdStatus::New;
        std::memcpy(evt.clord_id, "ORDER001", 8);
        evt.clord_id_len = 8;

        for (int i = 0; i < ITERATIONS; ++i) {
            ring.try_push(evt);
            core::ExecEvent out;
            ring.try_pop(out);
        }
    }

    // ---- 5. Mismatch computation (PerfCounterId::MismatchCompute) ----
    {
        core::OrderState os{};
        os.key = 12345;
        os.seen_internal = true;
        os.seen_dropcopy = true;
        os.internal_status = core::OrdStatus::PartiallyFilled;
        os.dropcopy_status = core::OrdStatus::PartiallyFilled;
        os.internal_cum_qty = 100;
        os.dropcopy_cum_qty = 100;
        os.internal_avg_px = 1000000;
        os.dropcopy_avg_px = 1000000;

        for (int i = 0; i < ITERATIONS; ++i) {
            core::MismatchMask mask = core::compute_mismatch(os);
            (void)mask;
        }
    }

    // ---- 6. Full reconciler process_event (PerfCounterId::ReconcilerProcessEvent) ----
    //         This exercises the entire pipeline: upsert, mismatch, timer wheel, etc.
    {
        using ExecRing = ingest::SpscRing<core::ExecEvent, 1u << 16>;

        std::atomic<bool> stop_flag{false};
        auto primary_ring = std::make_unique<ExecRing>();
        auto dropcopy_ring = std::make_unique<ExecRing>();
        auto divergence_ring = std::make_unique<core::DivergenceRing>();
        auto seq_gap_ring = std::make_unique<core::SequenceGapRing>();
        util::Arena arena(4ULL * 1024ULL * 1024ULL);
        core::OrderStateStore store(arena, 2048);
        core::ReconCounters counters{};
        util::WheelTimer timer_wheel(0);
        core::ReconConfig config{};

        core::Reconciler reconciler(
            stop_flag, *primary_ring, *dropcopy_ring, store, counters,
            *divergence_ring, *seq_gap_ring, &timer_wheel, config);

        for (int i = 0; i < ITERATIONS; ++i) {
            core::ExecEvent ev{};
            ev.source = (i & 1) ? core::Source::DropCopy : core::Source::Primary;
            ev.seq_num = static_cast<std::uint64_t>(i / 2 + 1);
            ev.ord_status = core::OrdStatus::New;
            ev.ingest_tsc = util::rdtsc();

            char buf[16];
            std::snprintf(buf, sizeof(buf), "CID%05d", i / 2);
            ev.set_clord_id(buf, 8);
            std::snprintf(buf, sizeof(buf), "EXE%05d", i);
            ev.set_exec_id(buf, 8);

            reconciler.process_event_for_test(ev);
        }
    }

    // ---- Dump all counters ----
    std::fprintf(stderr,
        "\n"
        "============================================================\n"
        "  FX Reconciler - Full Performance Report  (%d iterations)\n"
        "============================================================\n",
        ITERATIONS);

    registry.dump();

    std::fprintf(stderr,
        "============================================================\n\n");

    // Sanity assertions -- every counter should have been exercised
    EXPECT_GT(registry.get(util::PerfCounterId::ArenaAllocate).count, 0)
        << "ArenaAllocate was not exercised";
    EXPECT_GT(registry.get(util::PerfCounterId::HashTableUpsert).count, 0)
        << "HashTableUpsert was not exercised";
    EXPECT_GT(registry.get(util::PerfCounterId::HashTableLookup).count, 0)
        << "HashTableLookup was not exercised";
    EXPECT_GT(registry.get(util::PerfCounterId::TimerWheelSchedule).count, 0)
        << "TimerWheelSchedule was not exercised";
    EXPECT_GT(registry.get(util::PerfCounterId::TimerWheelPollExpired).count, 0)
        << "TimerWheelPollExpired was not exercised";
    EXPECT_GT(registry.get(util::PerfCounterId::SpscRingPush).count, 0)
        << "SpscRingPush was not exercised";
    EXPECT_GT(registry.get(util::PerfCounterId::SpscRingPop).count, 0)
        << "SpscRingPop was not exercised";
    EXPECT_GT(registry.get(util::PerfCounterId::MismatchCompute).count, 0)
        << "MismatchCompute was not exercised";
    EXPECT_GT(registry.get(util::PerfCounterId::ReconcilerProcessEvent).count, 0)
        << "ReconcilerProcessEvent was not exercised";
    EXPECT_GT(registry.get(util::PerfCounterId::EndToEndLatency).count, 0)
        << "EndToEndLatency was not exercised";
}

#endif // FX_PERF_ENABLED

} // namespace
