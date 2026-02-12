#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cmath>

namespace util {

// LatencyHistogram - Fixed-bucket histogram for recording nanosecond-precision latency samples.
//
// Design:
// - Linear bucketing: bucket_width = max_value_ns / num_buckets
// - Overflow bucket for values exceeding max_value_ns
// - Header-only, zero heap allocation after construction
// - Cache-line aligned for performance
// - Single-writer assumption (reconciler thread only)
//
// Template parameters:
// - NumBuckets: Number of histogram buckets (any positive value; power-of-2 not required)
// - MaxValueNs: Maximum value in nanoseconds (values above go to overflow bucket)
//
// Usage:
//   LatencyHistogram<256, 10000> hist;  // 256 buckets, max 10us
//   hist.record(1500);  // Record 1500ns sample
//   auto p99 = hist.percentile(0.99);
//   hist.print_report("Operation");
template <std::size_t NumBuckets = 256, std::uint64_t MaxValueNs = 100'000>
class alignas(64) LatencyHistogram {
    static_assert(NumBuckets > 0, "NumBuckets must be positive");
    static_assert(MaxValueNs > 0, "MaxValueNs must be positive");

public:
    constexpr LatencyHistogram() noexcept = default;

    // Record a latency sample in nanoseconds
    void record(std::uint64_t value_ns) noexcept {
        ++count_;
        
        // Update min/max
        if (count_ == 1) {
            min_ = value_ns;
            max_ = value_ns;
        } else {
            if (value_ns < min_) min_ = value_ns;
            if (value_ns > max_) max_ = value_ns;
        }
        
        // Update running sum for mean calculation
        sum_ += value_ns;
        
        // Determine bucket index
        std::size_t bucket_idx;
        if (value_ns >= MaxValueNs) {
            bucket_idx = NumBuckets;  // Overflow bucket
        } else {
            // Linear bucketing: bucket = value / bucket_width
            // To avoid division, we use multiplication: bucket = (value * NumBuckets) / MaxValueNs
            // Use 128-bit multiplication to prevent overflow
#if defined(__SIZEOF_INT128__)
            __uint128_t product = static_cast<__uint128_t>(value_ns) * NumBuckets;
            bucket_idx = static_cast<std::size_t>(product / MaxValueNs);
#else
            // Fallback: check if multiplication would overflow before performing it
            // This is slower but safe on platforms without 128-bit integers
            if (value_ns <= (UINT64_MAX / NumBuckets)) {
                bucket_idx = (value_ns * NumBuckets) / MaxValueNs;
            } else {
                // Use double for the calculation to avoid overflow
                const double ratio = static_cast<double>(value_ns) / MaxValueNs;
                bucket_idx = static_cast<std::size_t>(ratio * NumBuckets);
            }
#endif
            if (bucket_idx >= NumBuckets) {
                bucket_idx = NumBuckets - 1;  // Clamp to last regular bucket
            }
        }
        
        ++buckets_[bucket_idx];
    }

    // Calculate percentile (p in range [0.0, 1.0])
    // Returns the value at the given percentile or 0 if no samples
    [[nodiscard]] std::uint64_t percentile(double p) const noexcept {
        if (count_ == 0) return 0;
        
        // Clamp p to [0, 1]
        const double clamped_p = (p < 0.0) ? 0.0 : (p > 1.0) ? 1.0 : p;
        
        // Handle edge cases explicitly for reliability
        if (clamped_p == 0.0) {
            return min_;  // 0th percentile is the minimum
        }
        if (clamped_p == 1.0) {
            return max_;  // 100th percentile is the maximum
        }
        
        // Compute 1-based rank for interior percentiles
        const std::uint64_t target_rank = std::max<std::uint64_t>(1, 
            static_cast<std::uint64_t>(std::ceil(clamped_p * count_)));
        
        std::uint64_t cumulative = 0;
        
        // Scan buckets to find target percentile
        for (std::size_t i = 0; i <= NumBuckets; ++i) {
            cumulative += buckets_[i];
            if (cumulative >= target_rank) {
                // Return the upper bound of this bucket
                if (i == NumBuckets) {
                    // Overflow bucket - return max observed value
                    return max_;
                } else {
                    // Regular bucket - return bucket upper bound
                    return ((i + 1) * MaxValueNs) / NumBuckets;
                }
            }
        }
        
        return max_;  // Fallback
    }

    // Reset all counters
    void reset() noexcept {
        buckets_.fill(0);
        count_ = 0;
        sum_ = 0;
        min_ = 0;
        max_ = 0;
    }

    // Accessors
    [[nodiscard]] std::uint64_t count() const noexcept { return count_; }
    [[nodiscard]] std::uint64_t min() const noexcept { return min_; }
    [[nodiscard]] std::uint64_t max() const noexcept { return max_; }
    [[nodiscard]] std::uint64_t sum() const noexcept { return sum_; }
    [[nodiscard]] std::uint64_t mean() const noexcept {
        return count_ > 0 ? sum_ / count_ : 0;
    }

    // Print formatted report to stderr (consistent with PerfRegistry::dump())
    void print_report(const char* label) const noexcept {
        if (count_ == 0) {
            std::fprintf(stderr, "=== %s ===\nNo samples\n", label);
            return;
        }
        
        std::fprintf(stderr, "=== %s ===\n", label);
        std::fprintf(stderr, "P50: %lu ns | P99: %lu ns | P99.9: %lu ns\n",
                    static_cast<unsigned long>(percentile(0.50)),
                    static_cast<unsigned long>(percentile(0.99)),
                    static_cast<unsigned long>(percentile(0.999)));
    }

private:
    // Bucket array: NumBuckets regular buckets + 1 overflow bucket
    std::array<std::uint64_t, NumBuckets + 1> buckets_{};
    std::uint64_t count_{0};
    std::uint64_t sum_{0};
    std::uint64_t min_{0};
    std::uint64_t max_{0};
};

} // namespace util
