#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

#include "util/latency_histogram.hpp"

namespace {

using Histogram = util::LatencyHistogram;

class LatencyHistogramTest : public ::testing::Test {
protected:
    Histogram hist_;
};

// -------------------------------- Bucket mapping tests --------------------------------

TEST_F(LatencyHistogramTest, LinearBucketsMappedDirectly) {
    // Values 0–1023 should map 1:1 to bucket indices 0–1023
    for (std::uint64_t ns = 0; ns < Histogram::LINEAR_BUCKETS; ++ns) {
        EXPECT_EQ(Histogram::bucket_index(ns), ns) << "ns=" << ns;
    }
}

TEST_F(LatencyHistogramTest, FirstLogBucket) {
    // 1024 should be the first logarithmic bucket (index 1024)
    EXPECT_EQ(Histogram::bucket_index(1024), Histogram::LINEAR_BUCKETS);
    EXPECT_EQ(Histogram::bucket_index(2047), Histogram::LINEAR_BUCKETS);
}

TEST_F(LatencyHistogramTest, SecondLogBucket) {
    // [2048, 4096) -> bucket 1025
    EXPECT_EQ(Histogram::bucket_index(2048), Histogram::LINEAR_BUCKETS + 1);
    EXPECT_EQ(Histogram::bucket_index(4095), Histogram::LINEAR_BUCKETS + 1);
}

TEST_F(LatencyHistogramTest, BucketLowerBoundLinear) {
    for (std::size_t i = 0; i < Histogram::LINEAR_BUCKETS; ++i) {
        EXPECT_EQ(Histogram::bucket_lower_bound(i), i);
    }
}

TEST_F(LatencyHistogramTest, BucketLowerBoundLog) {
    // Log bucket 0 (index 1024): lower bound = 2^10 = 1024
    EXPECT_EQ(Histogram::bucket_lower_bound(Histogram::LINEAR_BUCKETS), 1024u);
    // Log bucket 1 (index 1025): lower bound = 2^11 = 2048
    EXPECT_EQ(Histogram::bucket_lower_bound(Histogram::LINEAR_BUCKETS + 1), 2048u);
    // Log bucket 10 (index 1034): lower bound = 2^20 = 1,048,576 (~1ms)
    EXPECT_EQ(Histogram::bucket_lower_bound(Histogram::LINEAR_BUCKETS + 10), 1u << 20);
}

TEST_F(LatencyHistogramTest, LargeValueClampedToLastBucket) {
    EXPECT_EQ(Histogram::bucket_index(Histogram::MAX_NS),
              Histogram::TOTAL_BUCKETS - 1);
    EXPECT_EQ(Histogram::bucket_index(UINT64_MAX),
              Histogram::TOTAL_BUCKETS - 1);
}

// -------------------------------- Recording tests --------------------------------

TEST_F(LatencyHistogramTest, EmptyHistogram) {
    EXPECT_EQ(hist_.count(), 0u);
    EXPECT_EQ(hist_.sum(), 0u);
    EXPECT_EQ(hist_.mean(), 0u);
    EXPECT_EQ(hist_.min_val(), 0u);
    EXPECT_EQ(hist_.max_val(), 0u);
    EXPECT_EQ(hist_.percentile(50.0), 0u);
}

TEST_F(LatencyHistogramTest, SingleSample) {
    hist_.record(500);
    EXPECT_EQ(hist_.count(), 1u);
    EXPECT_EQ(hist_.sum(), 500u);
    EXPECT_EQ(hist_.mean(), 500u);
    EXPECT_EQ(hist_.min_val(), 500u);
    EXPECT_EQ(hist_.max_val(), 500u);
    EXPECT_EQ(hist_.percentile(50.0), 500u);
    EXPECT_EQ(hist_.percentile(99.0), 500u);
    EXPECT_EQ(hist_.percentile(99.9), 500u);
}

TEST_F(LatencyHistogramTest, MultipleSamples) {
    hist_.record(100);
    hist_.record(200);
    hist_.record(300);

    EXPECT_EQ(hist_.count(), 3u);
    EXPECT_EQ(hist_.sum(), 600u);
    EXPECT_EQ(hist_.mean(), 200u);
    EXPECT_EQ(hist_.min_val(), 100u);
    EXPECT_EQ(hist_.max_val(), 300u);
}

TEST_F(LatencyHistogramTest, MinMaxTracking) {
    hist_.record(50);
    hist_.record(10);
    hist_.record(900);
    hist_.record(5);

    EXPECT_EQ(hist_.min_val(), 5u);
    EXPECT_EQ(hist_.max_val(), 900u);
}

// -------------------------------- Percentile accuracy tests --------------------------------

TEST_F(LatencyHistogramTest, PercentileUniformLinear) {
    // Record values 0–999 (all in linear range)
    for (std::uint64_t ns = 0; ns < 1000; ++ns) {
        hist_.record(ns);
    }

    EXPECT_EQ(hist_.count(), 1000u);

    // P50 should be ~500
    const auto p50 = hist_.percentile(50.0);
    EXPECT_GE(p50, 490u);
    EXPECT_LE(p50, 510u);

    // P99 should be ~990
    const auto p99 = hist_.percentile(99.0);
    EXPECT_GE(p99, 980u);
    EXPECT_LE(p99, 999u);

    // P0 should be 0
    EXPECT_EQ(hist_.percentile(0.0), 0u);

    // P100 should be last bucket populated
    const auto p100 = hist_.percentile(100.0);
    EXPECT_GE(p100, 990u);
}

TEST_F(LatencyHistogramTest, PercentileWithLogBuckets) {
    // Record 900 samples at 100 ns and 100 samples at 10000 ns (in log range)
    for (int i = 0; i < 900; ++i) {
        hist_.record(100);
    }
    for (int i = 0; i < 100; ++i) {
        hist_.record(10000);
    }

    EXPECT_EQ(hist_.count(), 1000u);

    // P50 should be 100 (linear bucket)
    EXPECT_EQ(hist_.percentile(50.0), 100u);

    // P90 should still be 100
    EXPECT_EQ(hist_.percentile(90.0), 100u);

    // P99 should reflect the 10000 ns samples (in log bucket starting at 8192)
    const auto p99 = hist_.percentile(99.0);
    EXPECT_GE(p99, 8192u);  // Lower bound of the log bucket containing 10000
}

TEST_F(LatencyHistogramTest, PercentileP999) {
    // Insert 10000 samples: 9990 at 500 ns, 10 at 50000 ns
    for (int i = 0; i < 9990; ++i) {
        hist_.record(500);
    }
    for (int i = 0; i < 10; ++i) {
        hist_.record(50000);
    }

    // P99.9 = ceil(10000 * 99.9 / 100) = 9990th sample
    // The first 9990 samples are all at 500, so P99.9 = 500
    const auto p999 = hist_.percentile(99.9);
    EXPECT_EQ(p999, 500u);

    // P99.95 = ceil(10000 * 99.95 / 100) = 9995th -> falls in the 50000 ns tail bucket
    const auto p9995 = hist_.percentile(99.95);
    // 50000 ns falls in log bucket starting at 32768 (2^15)
    EXPECT_GE(p9995, 32768u);
}

// -------------------------------- Reset test --------------------------------

TEST_F(LatencyHistogramTest, ResetClearsEverything) {
    hist_.record(100);
    hist_.record(200);
    hist_.record(300);

    hist_.reset();

    EXPECT_EQ(hist_.count(), 0u);
    EXPECT_EQ(hist_.sum(), 0u);
    EXPECT_EQ(hist_.mean(), 0u);
    EXPECT_EQ(hist_.min_val(), 0u);
    EXPECT_EQ(hist_.max_val(), 0u);
    EXPECT_EQ(hist_.percentile(50.0), 0u);
}

// -------------------------------- Dump test --------------------------------

TEST_F(LatencyHistogramTest, DumpProducesOutput) {
    hist_.record(100);
    hist_.record(200);
    hist_.record(300);

    char buf[512];
    std::size_t len = hist_.dump(buf, sizeof(buf));

    EXPECT_GT(len, 0u);

    // Should contain key labels
    std::string output(buf);
    EXPECT_NE(output.find("P50"), std::string::npos);
    EXPECT_NE(output.find("P99"), std::string::npos);
    EXPECT_NE(output.find("P99.9"), std::string::npos);
    EXPECT_NE(output.find("count=3"), std::string::npos);
}

TEST_F(LatencyHistogramTest, DumpEmptyHistogram) {
    char buf[512];
    std::size_t len = hist_.dump(buf, sizeof(buf));
    EXPECT_EQ(len, 0u);
}

// -------------------------------- Overhead measurement test --------------------------------

TEST_F(LatencyHistogramTest, RecordOverheadSubMicrosecond) {
    // Measure overhead of record() itself
    constexpr int WARMUP = 1000;
    constexpr int MEASURE = 100000;

    // Warm up
    for (int i = 0; i < WARMUP; ++i) {
        hist_.record(static_cast<std::uint64_t>(i));
    }
    hist_.reset();

    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < MEASURE; ++i) {
        hist_.record(static_cast<std::uint64_t>(i % 1024));
    }
    auto end = std::chrono::steady_clock::now();

    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const auto per_call_ns = elapsed_ns / MEASURE;

    // record() should be well under 1 microsecond (typically ~20-50 ns)
    EXPECT_LT(per_call_ns, 1000) << "record() overhead: " << per_call_ns << " ns/call";

    // Print for informational purposes
    std::fprintf(stderr, "[LatencyHistogramTest] record() overhead: %lld ns/call (%d iterations)\n",
                 static_cast<long long>(per_call_ns), MEASURE);
}

// -------------------------------- Edge cases --------------------------------

TEST_F(LatencyHistogramTest, ZeroLatency) {
    hist_.record(0);
    EXPECT_EQ(hist_.count(), 1u);
    EXPECT_EQ(hist_.min_val(), 0u);
    EXPECT_EQ(hist_.percentile(50.0), 0u);
}

TEST_F(LatencyHistogramTest, MaxLatency) {
    hist_.record(Histogram::MAX_NS);
    EXPECT_EQ(hist_.count(), 1u);
    EXPECT_EQ(hist_.max_val(), Histogram::MAX_NS);
}

TEST_F(LatencyHistogramTest, IdenticalSamples) {
    for (int i = 0; i < 1000; ++i) {
        hist_.record(42);
    }

    EXPECT_EQ(hist_.percentile(50.0), 42u);
    EXPECT_EQ(hist_.percentile(99.0), 42u);
    EXPECT_EQ(hist_.percentile(99.9), 42u);
    EXPECT_EQ(hist_.min_val(), 42u);
    EXPECT_EQ(hist_.max_val(), 42u);
}

} // namespace
