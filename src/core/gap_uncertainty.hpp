#pragma once

// ===== FX-7054: Gap Uncertainty Helpers =====
// This header provides functions for managing per-order gap uncertainty tracking.
// It must be included AFTER order_state.hpp and sequence_tracker.hpp are both
// fully defined, as these helpers operate on both types.
//
// Thread Safety: These functions are NOT thread-safe. Callers must ensure
// external synchronization when modifying OrderState and SequenceTracker
// from multiple threads. In typical HFT usage, each session is processed
// single-threaded, with its own tracker and order state store.

#include "core/order_state.hpp"
#include "core/sequence_tracker.hpp"

namespace core {

// ===== FX-7054: Gap uncertainty helper functions =====

// Helper to get the flag bit for a given source (reduces code duplication)
[[nodiscard]] constexpr std::uint8_t get_gap_flag_for_source(Source source) noexcept {
    return (source == Source::Primary)
        ? GapUncertaintyFlags::PRIMARY
        : GapUncertaintyFlags::DROPCOPY;
}

// Mark order as affected by a gap on the given source.
// Increments the tracker's orders_in_gap_count if newly marked.
// No-op if tracker's gap is not open.
//
// This function is idempotent: safe to call multiple times on the same order.
// Only takes effect when tracker.gap_open == true.
// 
// @param os The order state to mark
// @param source The source (Primary or DropCopy) that has the gap
// @param tracker The sequence tracker for the source (must have gap_open == true for effect)
inline void mark_gap_uncertainty(
    OrderState& os,
    Source source,
    SequenceTracker& tracker
) noexcept {
    // Early return: no effect when gap is not open
    if (!tracker.gap_open) {
        return;
    }

    const std::uint8_t flag = get_gap_flag_for_source(source);

    // Check if already marked for this source
    const bool was_marked = (os.gap_uncertainty_flags & flag) != 0;

    // Set the flag
    os.gap_uncertainty_flags |= flag;

    // Update epoch to latest gap epoch
    os.gap_suppression_epoch = tracker.gap_epoch;

    // Increment tracker count only if newly marked (avoid double-counting)
    if (!was_marked) {
        ++tracker.orders_in_gap_count;
    }
}

// Clear gap uncertainty for a specific source.
// Decrements tracker's orders_in_gap_count if was marked and tracker is provided.
// Safe to call even if order was not marked (no-op in that case).
//
// Note: The flag is cleared regardless of whether the tracker count is decremented.
// If tracker is nullptr or tracker->orders_in_gap_count is 0, this performs "best
// effort" cleanup - the flag is cleared but the count is not modified. This handles
// edge cases like clearing flags after close_gap() has reset the counter.
//
// @param os The order state to clear
// @param source The source (Primary or DropCopy) to clear
// @param tracker Optional tracker to decrement count. If nullptr, only clears the flag.
// @return true if the flag was previously set and cleared, false if it was already clear
[[nodiscard]] inline bool clear_gap_uncertainty(
    OrderState& os, 
    Source source, 
    SequenceTracker* tracker
) noexcept {
    const std::uint8_t flag = get_gap_flag_for_source(source);

    // Check if was marked
    const bool was_marked = (os.gap_uncertainty_flags & flag) != 0;

    if (!was_marked) {
        return false;  // Nothing to clear
    }

    // Clear the flag
    os.gap_uncertainty_flags &= static_cast<std::uint8_t>(~flag);

    // Decrement tracker count if tracker provided and count > 0
    // Note: Flag is cleared even if tracker is null or count is 0 (handles cleanup edge cases)
    if (tracker != nullptr && tracker->orders_in_gap_count > 0) {
        --tracker->orders_in_gap_count;
    }

    return true;
}

// Clear gap uncertainty for a specific source using a reference (convenience overload).
// Always decrements the tracker's count if the flag was set.
//
// @param os The order state to clear
// @param source The source (Primary or DropCopy) to clear
// @param tracker The tracker to decrement count
// @return true if the flag was previously set and cleared, false if it was already clear
[[nodiscard]] inline bool clear_gap_uncertainty(
    OrderState& os, 
    Source source, 
    SequenceTracker& tracker
) noexcept {
    return clear_gap_uncertainty(os, source, &tracker);
}

// ===== FX-7054 Part 2: Gap suppression check for reconciler =====
// Check if an order's reconciliation should be suppressed due to an open gap.
// Returns true if:
//   1. The order has the gap uncertainty flag set for this source, AND
//   2. The tracker's gap is currently open, AND
//   3. The order's gap_suppression_epoch matches the tracker's current gap_epoch
//
// The epoch check ensures that if a gap was closed and a new gap opened,
// orders marked during the old gap are NOT suppressed by the new gap.
//
// This is an O(1) check suitable for hot-path use.
//
// @param os The order state to check
// @param source The source (Primary or DropCopy) to check
// @param tracker The sequence tracker for the source
// @return true if this order should have divergence suppressed due to an open gap
[[nodiscard]] inline bool is_suppressed_by_gap(
    const OrderState& os,
    Source source,
    const SequenceTracker& tracker
) noexcept {
    // No suppression if gap is not open
    if (!tracker.gap_open) {
        return false;
    }
    
    const std::uint8_t flag = get_gap_flag_for_source(source);
    
    // Check if order has this source's gap flag set
    if ((os.gap_uncertainty_flags & flag) == 0) {
        return false;
    }
    
    // Verify epoch matches to handle gap closure/reopen scenarios.
    // The order must be marked with the CURRENT gap's epoch for the
    // specific source to be suppressed.
    std::uint64_t order_epoch = 0;
    switch (source) {
        case Source::Primary:
            order_epoch = os.primary_gap_epoch;
            break;
        case Source::DropCopy:
            order_epoch = os.dropcopy_gap_epoch;
            break;
    }
    return order_epoch == tracker.gap_epoch;
}

} // namespace core
