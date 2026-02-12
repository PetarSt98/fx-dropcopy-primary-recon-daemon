#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace util {

// LatencyHistogram provides lock-free nanosecond-precision latency recording
// with O(1) record and O(N_BUCKETS) percentile queries.
//
// Design:
// - Pre-allocated fixed-size bucket array, no heap allocation after construction
// - Logarithmic bucketing: linear 0–1023 ns (1 ns resolution), then log2 up to ~4 seconds
// - Single-writer optimised; readers may observe stale counts (relaxed atomics)
// - Suitable for hot-path instrumentation in HFT reconciliation loops
//
// Bucket layout:
//   [0, 1024)           -> buckets 0–1023   (1 ns per bucket, linear)
//   [1024, 2048)        -> bucket 1024      (1024 ns wide, log2 group 0)
//   [2048, 4096)        -> bucket 1025      (2048 ns wide, log2 group 1)
//   ...
//   [2^(K+10), 2^(K+11)) -> bucket 1024+K
//
// Total buckets: 1024 (linear) + 22 (log2, covering up to ~4 billion ns ≈ 4.29 s)
//
// Thread safety:
//   record()  – lock-free, safe from a single writer thread (relaxed store)
//   percentile() / dump() – may be called from any thread (relaxed load)
//   reset()   – must not race with record() or percentile()
class LatencyHistogram {
public:
    static constexpr std::size_t LINEAR_BUCKETS = 1024;
    static constexpr std::size_t LOG_BUCKETS    = 22;  // covers up to ~4.29 s
    static constexpr std::size_t TOTAL_BUCKETS  = LINEAR_BUCKETS + LOG_BUCKETS;

    // Maximum representable latency (ns): 2^(LOG_BUCKETS + 10) - 1 ≈ 4.29 billion ns
    static constexpr std::uint64_t MAX_NS = (1ULL << (LOG_BUCKETS + 10)) - 1;

    LatencyHistogram() noexcept { reset(); }

    // Record a single latency sample (nanoseconds).
    // Hot-path: O(1), no branches beyond bucket index calculation.
    void record(std::uint64_t ns) noexcept {
        const std::size_t idx = bucket_index(ns);
        // Relaxed: single-writer, readers tolerate stale counts
        buckets_[idx].fetch_add(1, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
        sum_.fetch_add(ns, std::memory_order_relaxed);
        // Update min
        std::uint64_t cur_min = min_.load(std::memory_order_relaxed);
        while (ns < cur_min &&
               !min_.compare_exchange_weak(cur_min, ns, std::memory_order_relaxed)) {
        }
        // Update max
        std::uint64_t cur_max = max_.load(std::memory_order_relaxed);
        while (ns > cur_max &&
               !max_.compare_exchange_weak(cur_max, ns, std::memory_order_relaxed)) {
        }
    }

    // Query the value at a given percentile [0.0, 100.0].
    // Returns the lower bound of the bucket containing the target rank.
    // O(TOTAL_BUCKETS) scan – not for hot path.
    [[nodiscard]] std::uint64_t percentile(double pct) const noexcept {
        const std::uint64_t n = count_.load(std::memory_order_relaxed);
        if (n == 0) {
            return 0;
        }
        // Target rank (1-based)
        const std::uint64_t target = static_cast<std::uint64_t>(
            std::ceil(static_cast<double>(n) * pct / 100.0));

        std::uint64_t cumulative = 0;
        for (std::size_t i = 0; i < TOTAL_BUCKETS; ++i) {
            cumulative += buckets_[i].load(std::memory_order_relaxed);
            if (cumulative >= target) {
                return bucket_lower_bound(i);
            }
        }
        // Should not reach here unless counts are stale
        return bucket_lower_bound(TOTAL_BUCKETS - 1);
    }

    // Total sample count.
    [[nodiscard]] std::uint64_t count() const noexcept {
        return count_.load(std::memory_order_relaxed);
    }

    // Sum of all recorded latencies (for computing mean).
    [[nodiscard]] std::uint64_t sum() const noexcept {
        return sum_.load(std::memory_order_relaxed);
    }

    // Mean latency (returns 0 if no samples).
    [[nodiscard]] std::uint64_t mean() const noexcept {
        const std::uint64_t n = count();
        return n > 0 ? sum() / n : 0;
    }

    // Observed minimum latency.
    [[nodiscard]] std::uint64_t min_val() const noexcept {
        const std::uint64_t n = count();
        return n > 0 ? min_.load(std::memory_order_relaxed) : 0;
    }

    // Observed maximum latency.
    [[nodiscard]] std::uint64_t max_val() const noexcept {
        return max_.load(std::memory_order_relaxed);
    }

    // Reset all buckets and counters.
    // NOT thread-safe with concurrent record() calls.
    void reset() noexcept {
        for (auto& b : buckets_) {
            b.store(0, std::memory_order_relaxed);
        }
        count_.store(0, std::memory_order_relaxed);
        sum_.store(0, std::memory_order_relaxed);
        min_.store(UINT64_MAX, std::memory_order_relaxed);
        max_.store(0, std::memory_order_relaxed);
    }

    // Dump a human-readable summary to the provided buffer.
    // Returns number of characters written (excluding NUL).
    std::size_t dump(char* buf, std::size_t buf_len) const noexcept {
        const auto n = count();
        if (n == 0 || buf_len == 0) {
            if (buf_len > 0) buf[0] = '\0';
            return 0;
        }
        int written = std::snprintf(
            buf, buf_len,
            "count=%llu mean=%llu ns min=%llu ns max=%llu ns | "
            "P50: %llu ns | P99: %llu ns | P99.9: %llu ns",
            static_cast<unsigned long long>(n),
            static_cast<unsigned long long>(mean()),
            static_cast<unsigned long long>(min_val()),
            static_cast<unsigned long long>(max_val()),
            static_cast<unsigned long long>(percentile(50.0)),
            static_cast<unsigned long long>(percentile(99.0)),
            static_cast<unsigned long long>(percentile(99.9)));
        return written > 0 ? static_cast<std::size_t>(written) : 0;
    }

    // Convenience: dump to stderr.
    void dump_to_stderr(const char* label) const noexcept {
        char buf[256];
        dump(buf, sizeof(buf));
        std::fprintf(stderr, "=== %s ===\n%s\n", label, buf);
    }

    // Bucket index for a given nanosecond value.
    [[nodiscard]] static std::size_t bucket_index(std::uint64_t ns) noexcept {
        if (ns < LINEAR_BUCKETS) {
            return static_cast<std::size_t>(ns);
        }
        // Log2 region: find the highest set bit position
        // For ns in [1024, 2^32), log2_group = floor(log2(ns)) - 10
#if defined(_MSC_VER)
        unsigned long bits = 0;
        _BitScanReverse64(&bits, ns | 1);
#else
        const unsigned bits = 63 - static_cast<unsigned>(__builtin_clzll(ns | 1));
#endif
        const unsigned log2_group = bits - 10;  // subtract linear region bits
        const std::size_t idx = LINEAR_BUCKETS + log2_group;
        return idx < TOTAL_BUCKETS ? idx : TOTAL_BUCKETS - 1;
    }

    // Lower bound (ns) of a given bucket.
    [[nodiscard]] static std::uint64_t bucket_lower_bound(std::size_t idx) noexcept {
        if (idx < LINEAR_BUCKETS) {
            return idx;
        }
        const std::size_t log_idx = idx - LINEAR_BUCKETS;
        return 1ULL << (log_idx + 10);
    }

private:
    std::array<std::atomic<std::uint64_t>, TOTAL_BUCKETS> buckets_{};
    std::atomic<std::uint64_t> count_{0};
    std::atomic<std::uint64_t> sum_{0};
    std::atomic<std::uint64_t> min_{UINT64_MAX};
    std::atomic<std::uint64_t> max_{0};
};

} // namespace util
