#pragma once

// ===== FX-7054: Gap Uncertainty Helpers =====
// This header provides functions for managing per-order gap uncertainty tracking.
// It must be included AFTER order_state.hpp and sequence_tracker.hpp are both
// fully defined, as these helpers operate on both types.

#include "core/order_state.hpp"
#include "core/sequence_tracker.hpp"

namespace core {

// ===== FX-7054: Gap uncertainty helper functions =====

// Mark order as affected by a gap on the given source.
// Increments the tracker's orders_in_gap_count if newly marked.
// No-op if tracker's gap is not open.
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

    const std::uint8_t flag = (source == Source::Primary)
        ? GapUncertaintyFlags::PRIMARY
        : GapUncertaintyFlags::DROPCOPY;

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
// @param os The order state to clear
// @param source The source (Primary or DropCopy) to clear
// @param tracker Optional tracker to decrement count. If nullptr, only clears the flag.
// @return true if the flag was previously set and cleared, false if it was already clear
[[nodiscard]] inline bool clear_gap_uncertainty(
    OrderState& os, 
    Source source, 
    SequenceTracker* tracker
) noexcept {
    const std::uint8_t flag = (source == Source::Primary)
        ? GapUncertaintyFlags::PRIMARY
        : GapUncertaintyFlags::DROPCOPY;

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

} // namespace core
