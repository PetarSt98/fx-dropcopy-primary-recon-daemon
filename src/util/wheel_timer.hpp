#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <type_traits>

#include "util/fixed_vec.hpp"
#include "util/tsc_calibration.hpp"
#include "core/order_state.hpp"  // For OrderKey

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
    // Configuration constants (in nanoseconds, converted to TSC at runtime)
    static constexpr std::size_t NUM_BUCKETS = 256;           // Power of 2 for fast masking
    static constexpr std::uint64_t TICK_NS = 1'000'000;       // 1ms per tick (in nanoseconds)
    static constexpr std::size_t BUCKET_CAPACITY = 1024;      // Max entries per bucket
    static constexpr std::uint64_t WHEEL_SPAN_NS = NUM_BUCKETS * TICK_NS;  // 256ms total coverage

    static_assert((NUM_BUCKETS & (NUM_BUCKETS - 1)) == 0, "NUM_BUCKETS must be power of 2");

    // Entry stored in each bucket
    struct Entry {
        core::OrderKey key{0};
        std::uint32_t generation{0};
        std::uint64_t deadline_tsc{0};  // Absolute deadline in TSC cycles
    };

    static_assert(std::is_trivially_copyable_v<Entry>, "Entry must be trivially copyable");

    // Statistics for monitoring
    struct Stats {
        std::uint64_t scheduled{0};         // Total schedules attempted
        std::uint64_t expired{0};           // Entries expired (callback invoked)
        std::uint64_t rescheduled{0};       // Far-future entries re-scheduled
        std::uint64_t overflow_dropped{0};  // Entries dropped due to bucket overflow
    };

    // Constructor - initializes wheel at given starting time (in TSC cycles)
    explicit WheelTimer(std::uint64_t start_tsc = 0) noexcept
        : tick_tsc_(ns_to_tsc(TICK_NS))
        , current_tick_(tick_tsc_ > 0 ? (start_tsc / tick_tsc_) : 0)
        , last_poll_tsc_(start_tsc) {
        // Ensure tick_tsc_ is at least 1 to prevent division by zero
        if (tick_tsc_ == 0) {
            tick_tsc_ = 1;
        }
    }

    // Schedule a deadline for an order.
    //
    // Parameters:
    //   key          - OrderKey identifying the order
    //   generation   - Current timer_generation from OrderState (for lazy cancellation)
    //   deadline_tsc - Absolute timestamp (in TSC cycles) when deadline expires
    //
    // Returns: true if scheduled successfully, false if bucket overflowed
    //
    // Note: Deadlines in the past are placed in the current bucket and will expire
    // on the next poll_expired() call.
    [[nodiscard]] bool schedule(core::OrderKey key, std::uint32_t generation,
                                std::uint64_t deadline_tsc) noexcept {
        ++stats_.scheduled;

        const std::uint64_t deadline_tick = deadline_tsc / tick_tsc_;
        std::uint64_t delta_ticks = (deadline_tick > current_tick_)
            ? (deadline_tick - current_tick_)
            : 0;

        // Clamp to wheel range (entry stores deadline_tsc for re-check)
        if (delta_ticks >= NUM_BUCKETS) {
            delta_ticks = NUM_BUCKETS - 1;
        }

        const std::size_t bucket_idx = (current_tick_ + delta_ticks) & (NUM_BUCKETS - 1);

        // Try to add to bucket
        if (!buckets_[bucket_idx].try_emplace_back(key, generation, deadline_tsc)) {
            ++stats_.overflow_dropped;
            return false;
        }

        return true;
    }

    // Poll for expired deadlines and invoke callback for each.
    //
    // Parameters:
    //   now_tsc    - Current timestamp (in TSC cycles)
    //   on_expired - Callback invoked as on_expired(OrderKey key, uint32_t generation)
    //                Caller MUST check generation against OrderState.timer_generation
    //                to detect if timer was cancelled (generation mismatch = skip)
    //
    // Complexity: O(number of expired entries), NOT O(total scheduled)
    template <typename F>
    void poll_expired(std::uint64_t now_tsc, F&& on_expired) noexcept {
        const std::uint64_t now_tick = now_tsc / tick_tsc_;

        // Process all ticks from last poll to now
        while (current_tick_ < now_tick) {
            const std::size_t bucket_idx = current_tick_ & (NUM_BUCKETS - 1);
            auto& bucket = buckets_[bucket_idx];

            // Process entries - some may need re-scheduling (far-future)
            for (std::size_t i = 0; i < bucket.size(); ) {
                const auto& entry = bucket[i];

                if (entry.deadline_tsc <= now_tsc) {
                    // Deadline reached - invoke callback
                    on_expired(entry.key, entry.generation);
                    ++stats_.expired;
                    bucket.swap_erase(i);
                    // Don't increment i - new entry now at position i
                } else {
                    // Far-future entry not yet due - re-schedule
                    const auto key = entry.key;
                    const auto gen = entry.generation;
                    const auto deadline = entry.deadline_tsc;
                    bucket.swap_erase(i);

                    // Re-schedule (may fail if target bucket full)
                    if (schedule(key, gen, deadline)) {
                        ++stats_.rescheduled;
                    }
                    // Always decrement scheduled to avoid double-counting, even if reschedule failed
                    --stats_.scheduled;
                    // Don't increment i
                }
            }

            ++current_tick_;
        }

        last_poll_tsc_ = now_tsc;
    }

    // Advance the wheel without processing expirations.
    // Use poll_expired() in normal operation; this is for testing/edge cases.
    void advance(std::uint64_t now_tsc) noexcept {
        const std::uint64_t now_tick = now_tsc / tick_tsc_;
        if (now_tick > current_tick_) {
            current_tick_ = now_tick;
        }
        last_poll_tsc_ = now_tsc;
    }

    // Reset the wheel (e.g., for end-of-day reset)
    void reset(std::uint64_t start_tsc = 0) noexcept {
        for (auto& bucket : buckets_) {
            bucket.clear();
        }
        current_tick_ = start_tsc / tick_tsc_;
        last_poll_tsc_ = start_tsc;
        stats_ = Stats{};
    }

    // Accessors
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }
    [[nodiscard]] std::uint64_t current_tick() const noexcept { return current_tick_; }
    [[nodiscard]] std::uint64_t last_poll_tsc() const noexcept { return last_poll_tsc_; }
    [[nodiscard]] std::uint64_t tick_tsc() const noexcept { return tick_tsc_; }

    // Count total entries across all buckets (O(NUM_BUCKETS), for debugging/monitoring)
    [[nodiscard]] std::size_t total_pending() const noexcept {
        std::size_t total = 0;
        for (const auto& bucket : buckets_) {
            total += bucket.size();
        }
        return total;
    }

private:
    using Bucket = FixedCapacityVec<Entry, BUCKET_CAPACITY>;

    std::uint64_t tick_tsc_{1};  // TSC cycles per tick (converted from TICK_NS at construction)
    std::array<Bucket, NUM_BUCKETS> buckets_{};
    std::uint64_t current_tick_{0};
    std::uint64_t last_poll_tsc_{0};
    Stats stats_{};
};

} // namespace util
