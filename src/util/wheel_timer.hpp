#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <type_traits>

#include "util/fixed_vec.hpp"
#include "util/tsc_calibration.hpp"
#include "util/perf_macros.hpp"
#include "core/order_state.hpp"  // For OrderKey

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    #include <intrin.h>
#endif

namespace util {

// WheelTimer provides O(1) deadline scheduling for the reconciliation grace period.
//
// Design:
// - Single-level timing wheel with NUM_BUCKETS slots
// - Each bucket covers TICK_NS nanoseconds (converted to TSC cycles at runtime)
// - Total coverage = NUM_BUCKETS * TICK_NS (e.g., 256 * 1ms = 256ms)
// - Deadlines beyond wheel range are placed in future bucket, re-checked on expiry
// - All timestamps are in TSC cycles for HFT performance
//
// Cancellation:
// - Uses generation counter pattern (no explicit cancel API)
// - Caller stores (key, generation) when scheduling
// - On expiry, caller checks if generation matches current OrderState.timer_generation
// - If mismatch, the timer was "cancelled" (generation was incremented)
//
// Thread safety: None. Single-writer only (reconciler thread).
//
// Memory: All storage is pre-allocated. No heap allocations after construction.
class WheelTimer {
public:
    static constexpr std::size_t NUM_BUCKETS = 256;
    static constexpr std::uint64_t TICK_NS = 1'000'000;        // 1ms per tick
    static constexpr std::size_t BUCKET_CAPACITY = 1024;
    static constexpr std::uint64_t WHEEL_SPAN_NS = NUM_BUCKETS * TICK_NS;

    static_assert((NUM_BUCKETS & (NUM_BUCKETS - 1)) == 0, "NUM_BUCKETS must be power of 2");

    struct Entry {
        core::OrderKey key{0};
        std::uint32_t generation{0};
        std::uint64_t deadline_tsc{0};
    };

    static_assert(std::is_trivially_copyable_v<Entry>, "Entry must be trivially copyable");

    struct Stats {
        std::uint64_t scheduled{0};
        std::uint64_t expired{0};
        std::uint64_t rescheduled{0};
        std::uint64_t overflow_dropped{0};
    };

    static constexpr std::uint64_t DEFAULT_TICK_TSC =
        (TICK_NS * TscCalibration::DEFAULT_TSC_FREQ_HZ) / TscCalibration::NS_PER_SEC;

    explicit WheelTimer(std::uint64_t start_tsc = 0) noexcept
        : tick_tsc_(ns_to_tsc(TICK_NS))
        , tick_reciprocal_(0)
        , current_tick_(0)
        , last_poll_tsc_(start_tsc)
    {
        if (tick_tsc_ == 0) {
            tick_tsc_ = DEFAULT_TICK_TSC;
        }
        tick_reciprocal_ = compute_tick_reciprocal(tick_tsc_);
        current_tick_ = tsc_to_tick(start_tsc);
    }

    // Schedule a deadline for an order.
    // Returns: true if scheduled successfully, false if bucket overflowed.
    // Deadlines in the past are placed in the current bucket and expire on next poll.
    [[nodiscard]] bool schedule(core::OrderKey key, std::uint32_t generation,
                                std::uint64_t deadline_tsc) noexcept {
        PERF_SCOPE(::util::PerfCounterId::TimerWheelSchedule);

        ++stats_.scheduled;

        const std::uint64_t deadline_tick = tsc_to_tick(deadline_tsc);
        std::uint64_t delta_ticks = (deadline_tick > current_tick_)
            ? (deadline_tick - current_tick_)
            : 0;

        if (delta_ticks >= NUM_BUCKETS) {
            delta_ticks = NUM_BUCKETS - 1;
        }

        const std::size_t bucket_idx = (current_tick_ + delta_ticks) & (NUM_BUCKETS - 1);

        if (!buckets_[bucket_idx].try_emplace_back(key, generation, deadline_tsc)) {
            ++stats_.overflow_dropped;
            return false;
        }

        return true;
    }

    // Poll for expired deadlines and invoke callback for each.
    // Callback signature: on_expired(OrderKey key, uint32_t generation)
    // Caller MUST check generation against OrderState.timer_generation.
    // Complexity: O(number of expired entries), NOT O(total scheduled).
    template <typename F>
    void poll_expired(std::uint64_t now_tsc, F&& on_expired) noexcept {
        PERF_SCOPE(::util::PerfCounterId::TimerWheelPollExpired);

        const std::uint64_t now_tick = tsc_to_tick(now_tsc);

        while (current_tick_ < now_tick) {
            const std::size_t bucket_idx = current_tick_ & (NUM_BUCKETS - 1);
            auto& bucket = buckets_[bucket_idx];

            for (std::size_t i = 0; i < bucket.size(); ) {
                const auto& entry = bucket[i];

                if (entry.deadline_tsc <= now_tsc) {
                    on_expired(entry.key, entry.generation);
                    ++stats_.expired;
                    bucket.swap_erase(i);
                } else {
                    // Far-future entry not yet due -- move to correct bucket.
                    // Inlined (not via schedule()) to avoid double-counting stats
                    // and to guarantee forward progress (delta >= 1).
                    const auto rkey = entry.key;
                    const auto rgen = entry.generation;
                    const auto rdeadline = entry.deadline_tsc;
                    bucket.swap_erase(i);

                    const std::uint64_t dtick = tsc_to_tick(rdeadline);
                    std::uint64_t delta = (dtick > current_tick_)
                        ? (dtick - current_tick_)
                        : 1;   // safety: force forward progress on rounding edge cases
                    if (delta >= NUM_BUCKETS) delta = NUM_BUCKETS - 1;

                    const std::size_t target = (current_tick_ + delta) & (NUM_BUCKETS - 1);
                    if (buckets_[target].try_emplace_back(rkey, rgen, rdeadline)) {
                        ++stats_.rescheduled;
                    } else {
                        ++stats_.overflow_dropped;
                    }
                }
            }

            ++current_tick_;
        }

        last_poll_tsc_ = now_tsc;
    }

    void advance(std::uint64_t now_tsc) noexcept {
        const std::uint64_t now_tick = tsc_to_tick(now_tsc);
        if (now_tick > current_tick_) {
            current_tick_ = now_tick;
        }
        last_poll_tsc_ = now_tsc;
    }

    void reset(std::uint64_t start_tsc = 0) noexcept {
        for (auto& bucket : buckets_) {
            bucket.clear();
        }
        current_tick_ = tsc_to_tick(start_tsc);
        last_poll_tsc_ = start_tsc;
        stats_ = Stats{};
    }

    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t current_tick() const noexcept { return current_tick_; }
    [[nodiscard]] std::uint64_t last_poll_tsc() const noexcept { return last_poll_tsc_; }
    [[nodiscard]] std::uint64_t tick_tsc() const noexcept { return tick_tsc_; }

    [[nodiscard]] std::size_t total_pending() const noexcept {
        std::size_t total = 0;
        for (const auto& bucket : buckets_) {
            total += bucket.size();
        }
        return total;
    }

private:
    using Bucket = FixedCapacityVec<Entry, BUCKET_CAPACITY>;

    // Pre-compute ceil(2^64 / d) for fast approximate division via mulhi64.
    // Using ceiling ensures tsc_to_tick never undercounts (at most overcounts
    // by 1 tick), which is safe -- exact deadline_tsc comparison handles precision.
    static std::uint64_t compute_tick_reciprocal(std::uint64_t d) noexcept {
        if (d <= 1) return ~std::uint64_t{0};
#if defined(__SIZEOF_INT128__)
        const __uint128_t num = static_cast<__uint128_t>(1) << 64;
        return static_cast<std::uint64_t>((num + d - 1) / d);
#elif defined(_MSC_VER) && defined(_M_X64)
        std::uint64_t remainder;
        std::uint64_t quotient = _udiv128(1, 0, d, &remainder);
        return remainder != 0 ? quotient + 1 : quotient;
#else
        return 0;  // Signals fallback to plain division
#endif
    }

    // Fast approximate division: x / tick_tsc_ via multiply-high.
    // ~4-5 cycles (mul+shift) vs ~20-40 cycles (div64) on x86-64.
    [[nodiscard]] std::uint64_t tsc_to_tick(std::uint64_t tsc) const noexcept {
#if defined(__SIZEOF_INT128__)
        if (tick_reciprocal_ != 0) {
            return static_cast<std::uint64_t>(
                (static_cast<__uint128_t>(tsc) * tick_reciprocal_) >> 64
            );
        }
#elif defined(_MSC_VER) && defined(_M_X64)
        if (tick_reciprocal_ != 0) {
            std::uint64_t high;
            _umul128(tsc, tick_reciprocal_, &high);
            return high;
        }
#endif
        return tsc / tick_tsc_;
    }

    // Hot fields on same cache line (accessed every schedule/poll)
    std::uint64_t tick_tsc_{1};
    std::uint64_t tick_reciprocal_{0};
    std::uint64_t current_tick_{0};
    std::uint64_t last_poll_tsc_{0};
    Stats stats_{};

    // Cold bulk storage (~6MB, accessed per-bucket)
    std::array<Bucket, NUM_BUCKETS> buckets_{};
};

} // namespace util
