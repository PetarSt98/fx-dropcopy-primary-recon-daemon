#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <cassert>

#include "core/exec_event.hpp"
#include "core/order_lifecycle.hpp"
#include "core/recon_state.hpp"
#include "util/arena.hpp"
#include "util/tsc_calibration.hpp"

namespace core {

using OrderKey = std::uint64_t;

inline OrderKey make_order_key(const ExecEvent& evt) noexcept {
    // FNV-1a 64-bit hash over ClOrdID bytes; deterministic and stable.
    static constexpr OrderKey fnv_offset_basis = 14695981039346656037ULL;
    static constexpr OrderKey fnv_prime = 1099511628211ULL;

    OrderKey hash = fnv_offset_basis;
    for (std::size_t i = 0; i < evt.clord_id_len; ++i) {
        hash ^= static_cast<std::uint8_t>(evt.clord_id[i]);
        hash *= fnv_prime;
    }
    return hash;
}

struct OrderState {
    OrderKey key{0};

    // Internal (primary) view.
    OrdStatus internal_status{OrdStatus::Unknown};
    std::int64_t internal_cum_qty{0};
    std::int64_t internal_avg_px{0};
    std::uint64_t last_internal_ts{0};
    char last_internal_exec_id[ExecEvent::id_capacity]{};
    std::uint8_t last_internal_exec_id_len{0};

    // Drop-copy view.
    OrdStatus dropcopy_status{OrdStatus::Unknown};
    std::int64_t dropcopy_cum_qty{0};
    std::int64_t dropcopy_avg_px{0};
    std::uint64_t last_dropcopy_ts{0};
    char last_dropcopy_exec_id[ExecEvent::id_capacity]{};
    std::uint8_t last_dropcopy_exec_id_len{0};

    // Bookkeeping.
    bool seen_internal{false};
    bool seen_dropcopy{false};
    bool has_divergence{false};
    bool has_gap{false};
    std::uint32_t divergence_count{0};

    // ===== Reconciliation overlay (FX-7051) =====
    // Tracks reconciliation lifecycle separately from FIX execution state.
    std::uint64_t primary_last_seen_tsc{0};
    std::uint64_t dropcopy_last_seen_tsc{0};
    std::uint64_t mismatch_first_seen_tsc{0};
    std::uint64_t recon_deadline_tsc{0};

    ReconState recon_state{ReconState::Unknown};
    MismatchMask current_mismatch{};      // 1-byte mask

    std::uint32_t timer_generation{0};    // generation-based lazy cancel
    std::uint16_t gap_suppression_epoch{0};  // Upgraded to uint16_t to avoid wrap-around issues

    // ===== FX-7054: Per-order gap uncertainty flags =====
    // Bitmask indicating which session gaps affect this order's reconciliation
    // Bit 0: Primary session gap uncertainty
    // Bit 1: DropCopy session gap uncertainty
    // Bits 2-7: Reserved for additional sessions
    std::uint8_t gap_uncertainty_flags{0};

    // ===== Divergence emission tracking (FX-7053) =====
    // These fields support idempotent divergence emission to avoid flooding
    // the downstream divergence queue with repeated identical events.
    std::uint64_t last_divergence_emit_tsc{0};  // When last divergence was emitted (0 = never)
    MismatchMask last_emitted_mismatch{};       // What mismatch bits were last emitted
    std::uint32_t divergence_emit_count{0};     // Total divergences emitted for this order (lifetime)
};

inline OrderState* create_order_state(util::Arena& arena, OrderKey key) noexcept {
    void* mem = arena.allocate(sizeof(OrderState), alignof(OrderState));
    if (!mem) {
        return nullptr;
    }
    std::memset(mem, 0, sizeof(OrderState));
    auto* state = static_cast<OrderState*>(mem);
    state->key = key;
    state->internal_status = OrdStatus::Unknown;
    state->dropcopy_status = OrdStatus::Unknown;
    state->recon_state = ReconState::Unknown;
    return state;
}

inline std::uint64_t select_event_timestamp(const ExecEvent& ev) noexcept {
    return ev.transact_time != 0 ? ev.transact_time : ev.sending_time;
}

inline std::uint8_t bounded_exec_id_length(std::size_t len) noexcept {
    return static_cast<std::uint8_t>(len > ExecEvent::id_capacity ? ExecEvent::id_capacity : len);
}

// Applies an internal (primary session) ExecEvent to the OrderState.
// Returns true if applied successfully, false if the transition was invalid.
inline bool apply_internal_exec(OrderState& state, const ExecEvent& ev) noexcept {
#ifndef NDEBUG
    assert(make_order_key(ev) == state.key);
#endif
    const OrdStatus next = ev.ord_status;
    if (!apply_status_transition(state.internal_status, next)) {
        state.has_divergence = true;
        ++state.divergence_count;
        return false;
    }

    state.internal_cum_qty = ev.cum_qty;
    state.internal_avg_px = ev.price_micro;
    state.last_internal_ts = select_event_timestamp(ev);
    const auto len = bounded_exec_id_length(ev.exec_id_len);
    if (len > 0) {
        std::memcpy(state.last_internal_exec_id, ev.exec_id, len);
    }
    state.last_internal_exec_id_len = len;
    state.seen_internal = true;
    return true;
}

// Applies a drop-copy ExecEvent to the OrderState.
inline bool apply_dropcopy_exec(OrderState& state, const ExecEvent& ev) noexcept {
#ifndef NDEBUG
    assert(make_order_key(ev) == state.key);
#endif
    const OrdStatus next = ev.ord_status;
    if (!apply_status_transition(state.dropcopy_status, next)) {
        state.has_divergence = true;
        ++state.divergence_count;
        return false;
    }

    state.dropcopy_cum_qty = ev.cum_qty;
    state.dropcopy_avg_px = ev.price_micro;
    state.last_dropcopy_ts = select_event_timestamp(ev);
    const auto len = bounded_exec_id_length(ev.exec_id_len);
    if (len > 0) {
        std::memcpy(state.last_dropcopy_exec_id, ev.exec_id, len);
    }
    state.last_dropcopy_exec_id_len = len;
    state.seen_dropcopy = true;
    return true;
}

// Pure, noexcept mismatch computation for hot path.
// No tolerance parameters in this version (tolerance/config comes in FX-7053/FX-7200).
[[nodiscard]] inline MismatchMask compute_mismatch(const OrderState& os) noexcept {
    MismatchMask m{};

    // Existence mismatch: if one side seen but not the other
    if (os.seen_internal != os.seen_dropcopy) {
        m.set(MismatchMask::EXISTENCE);
        return m;  // Early return on existence mismatch
    }

    // If neither side seen, return empty mask
    if (!os.seen_internal && !os.seen_dropcopy) {
        return m;
    }

    // Both sides seen: compare fields

    // Status mismatch
    if (os.internal_status != os.dropcopy_status) {
        m.set(MismatchMask::STATUS);
    }

    // CumQty mismatch
    if (os.internal_cum_qty != os.dropcopy_cum_qty) {
        m.set(MismatchMask::CUM_QTY);
    }

    // LEAVES_QTY: Not computed in v1 because total order qty (order_qty) is not tracked
    // in OrderState. Future implementation will either compute leaves_qty
    // as (order_qty - cum_qty) or compare directly-reported leaves_qty values
    // from both feeds once order_qty tracking is added.

    // AvgPx mismatch
    if (os.internal_avg_px != os.dropcopy_avg_px) {
        m.set(MismatchMask::AVG_PX);
    }

    // ExecID mismatch: compare lengths first, then content if both are populated
    if (os.last_internal_exec_id_len != os.last_dropcopy_exec_id_len) {
        m.set(MismatchMask::EXEC_ID);
    } else if (os.last_internal_exec_id_len > 0 && os.last_dropcopy_exec_id_len > 0) {
        if (std::memcmp(os.last_internal_exec_id, os.last_dropcopy_exec_id, 
                        os.last_internal_exec_id_len) != 0) {
            m.set(MismatchMask::EXEC_ID);
        }
    }

    return m;
}

// Mismatch computation with tolerance parameters (FX-7053 Part 3).
// Tolerances allow for minor differences without triggering mismatches.
[[nodiscard]] inline MismatchMask compute_mismatch(
    const OrderState& os,
    std::int64_t qty_tolerance,
    std::int64_t px_tolerance
) noexcept {
    MismatchMask m{};

    // Existence mismatch: if one side seen but not the other
    if (os.seen_internal != os.seen_dropcopy) {
        m.set(MismatchMask::EXISTENCE);
        return m;  // Early return on existence mismatch
    }

    // If neither side seen, return empty mask
    if (!os.seen_internal && !os.seen_dropcopy) {
        return m;
    }

    // Both sides seen: compare fields

    // Status mismatch (no tolerance for status)
    if (os.internal_status != os.dropcopy_status) {
        m.set(MismatchMask::STATUS);
    }

    // CumQty mismatch with tolerance
    const auto qty_diff = std::llabs(os.internal_cum_qty - os.dropcopy_cum_qty);
    if (qty_diff > qty_tolerance) {
        m.set(MismatchMask::CUM_QTY);
    }

    // AvgPx mismatch with tolerance
    const auto px_diff = std::llabs(os.internal_avg_px - os.dropcopy_avg_px);
    if (px_diff > px_tolerance) {
        m.set(MismatchMask::AVG_PX);
    }

    // ExecID mismatch: compare lengths first, then content if both are populated
    if (os.last_internal_exec_id_len != os.last_dropcopy_exec_id_len) {
        m.set(MismatchMask::EXEC_ID);
    } else if (os.last_internal_exec_id_len > 0 && os.last_dropcopy_exec_id_len > 0) {
        if (std::memcmp(os.last_internal_exec_id, os.last_dropcopy_exec_id, 
                        os.last_internal_exec_id_len) != 0) {
            m.set(MismatchMask::EXEC_ID);
        }
    }

    return m;
}

// Check if a divergence should be emitted or deduplicated.
// Returns true if enough time has passed since last emission with same mismatch.
// Returns false if this would be a duplicate (suppress emission).
// Note: dedup_window_ns is in nanoseconds but now_tsc is in TSC cycles
[[nodiscard]] inline bool should_emit_divergence(
    const OrderState& os,
    MismatchMask current_mismatch,
    std::uint64_t now_tsc,
    std::uint64_t dedup_window_ns
) noexcept {
    // Always emit if mismatch changed
    if (current_mismatch != os.last_emitted_mismatch) {
        return true;
    }

    // Always emit if never emitted before
    if (os.last_divergence_emit_tsc == 0) {
        return true;
    }

    // Deduplicate if same mismatch within window
    // Guard against underflow if now_tsc < last_divergence_emit_tsc (e.g., TSC rollover)
    if (now_tsc < os.last_divergence_emit_tsc) {
        return true;  // Emit to be safe on clock anomaly
    }
    // Convert nanoseconds window to TSC cycles for correct comparison
    const std::uint64_t dedup_window_tsc = util::ns_to_tsc(dedup_window_ns);
    return (now_tsc - os.last_divergence_emit_tsc) >= dedup_window_tsc;
}

// Record that a divergence was emitted.
// Call this after successfully emitting to update tracking fields.
inline void record_divergence_emission(
    OrderState& os,
    MismatchMask emitted_mismatch,
    std::uint64_t emit_tsc
) noexcept {
    os.last_divergence_emit_tsc = emit_tsc;
    os.last_emitted_mismatch = emitted_mismatch;
    ++os.divergence_emit_count;
}

static_assert(sizeof(OrderState) <= 256, "OrderState exceeds cache-friendly size limit");
static_assert(std::is_trivially_copyable_v<OrderState>, "OrderState must remain trivially copyable");

// ===== FX-7054: Gap uncertainty flag bits =====
namespace GapUncertaintyFlags {
    constexpr std::uint8_t NONE     = 0u;
    constexpr std::uint8_t PRIMARY  = 1u << 0;  // Bit 0
    constexpr std::uint8_t DROPCOPY = 1u << 1;  // Bit 1
    // Bits 2-7 reserved for future multi-session support
}

// Forward declaration for SequenceTracker (defined in sequence_tracker.hpp)
struct SequenceTracker;

// ===== FX-7054: Gap uncertainty helper functions =====

// Mark order as affected by a gap on the given source.
// Increments the tracker's orders_in_gap_count if newly marked.
inline void mark_gap_uncertainty(
    OrderState& os,
    Source source,
    SequenceTracker& tracker
) noexcept;

// Check if order has any gap uncertainty flags set
[[nodiscard]] inline bool has_gap_uncertainty(const OrderState& os) noexcept {
    return os.gap_uncertainty_flags != GapUncertaintyFlags::NONE;
}

// Check if order has gap uncertainty for specific source
[[nodiscard]] inline bool has_gap_uncertainty_for(const OrderState& os, Source source) noexcept {
    const std::uint8_t flag = (source == Source::Primary)
        ? GapUncertaintyFlags::PRIMARY
        : GapUncertaintyFlags::DROPCOPY;
    return (os.gap_uncertainty_flags & flag) != 0;
}

// Clear gap uncertainty for a specific source.
// Decrements tracker's orders_in_gap_count if was marked.
inline void clear_gap_uncertainty(OrderState& os, Source source, SequenceTracker* tracker = nullptr) noexcept;

// Clear all gap uncertainty (e.g., when order is confirmed matched)
inline void clear_all_gap_uncertainty(OrderState& os) noexcept {
    os.gap_uncertainty_flags = GapUncertaintyFlags::NONE;
    // Note: gap_suppression_epoch preserved for historical tracking
}

} // namespace core

// ===== FX-7054: Inline implementation dependency resolution =====
// Include sequence_tracker.hpp after OrderState is fully defined to avoid circular dependency.
// This pattern is intentional: sequence_tracker.hpp includes exec_event.hpp (for Source),
// while order_state.hpp needs SequenceTracker for helper functions. By placing the include
// after OrderState definition and before the inline implementations that use SequenceTracker,
// we break the circular dependency while keeping all code inline (HFT requirement).
#include "core/sequence_tracker.hpp"

namespace core {

// ===== FX-7054: Inline implementations that depend on SequenceTracker =====

inline void mark_gap_uncertainty(
    OrderState& os,
    Source source,
    SequenceTracker& tracker
) noexcept {
    if (!tracker.gap_open) return;

    const std::uint8_t flag = (source == Source::Primary)
        ? GapUncertaintyFlags::PRIMARY
        : GapUncertaintyFlags::DROPCOPY;

    // Check if already marked
    const bool was_marked = (os.gap_uncertainty_flags & flag) != 0;

    // Set the flag
    os.gap_uncertainty_flags |= flag;

    // Update epoch to latest
    os.gap_suppression_epoch = tracker.gap_epoch;

    // Increment tracker count if newly marked
    if (!was_marked) {
        ++tracker.orders_in_gap_count;
    }
}

inline void clear_gap_uncertainty(OrderState& os, Source source, SequenceTracker* tracker) noexcept {
    const std::uint8_t flag = (source == Source::Primary)
        ? GapUncertaintyFlags::PRIMARY
        : GapUncertaintyFlags::DROPCOPY;

    // Check if was marked
    const bool was_marked = (os.gap_uncertainty_flags & flag) != 0;

    // Clear the flag
    os.gap_uncertainty_flags &= static_cast<std::uint8_t>(~flag);

    // Decrement tracker count if was marked and tracker provided
    if (was_marked && tracker) {
        // Debug assertion: count should be > 0 if order was marked
        // If this fires, it indicates an inconsistency in gap tracking state
        assert(tracker->orders_in_gap_count > 0 && 
               "clear_gap_uncertainty: underflow detected - count already 0 but order was marked");
        if (tracker->orders_in_gap_count > 0) {
            --tracker->orders_in_gap_count;
        }
    }
}

} // namespace core
