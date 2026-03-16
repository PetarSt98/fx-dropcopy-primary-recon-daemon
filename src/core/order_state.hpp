#pragma once

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>

#include "core/exec_event.hpp"
#include "core/order_lifecycle.hpp"
#include "core/recon_state.hpp"
#include "util/arena.hpp"
#include "util/tsc_calibration.hpp"
#include "util/perf_macros.hpp"

namespace core {

// Safe absolute difference computation for signed integers.
// Avoids undefined behavior from signed overflow and std::llabs(LLONG_MIN).
// Casts to unsigned BEFORE subtraction to ensure well-defined unsigned arithmetic.
[[nodiscard]] inline std::uint64_t safe_abs_diff(std::int64_t a, std::int64_t b) noexcept {
    const std::uint64_t ua = static_cast<std::uint64_t>(a);
    const std::uint64_t ub = static_cast<std::uint64_t>(b);
    return (a >= b) ? (ua - ub) : (ub - ua);
}

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

    // === Hot comparison fields (compute_mismatch reads all of these) ===
    // Grouped together so the primary mismatch check fits in a single cache line.
    OrdStatus internal_status{OrdStatus::Unknown};
    OrdStatus dropcopy_status{OrdStatus::Unknown};
    bool seen_internal{false};
    bool seen_dropcopy{false};

    std::int64_t internal_cum_qty{0};
    std::int64_t dropcopy_cum_qty{0};
    std::int64_t internal_avg_px{0};
    std::int64_t dropcopy_avg_px{0};
    std::uint64_t last_internal_ts{0};
    std::uint64_t last_dropcopy_ts{0};

    // === Exec ID comparison (hot but larger, checked last in mismatch) ===
    char last_internal_exec_id[ExecEvent::id_capacity]{};
    std::uint8_t last_internal_exec_id_len{0};
    char last_dropcopy_exec_id[ExecEvent::id_capacity]{};
    std::uint8_t last_dropcopy_exec_id_len{0};

    // === Reconciliation overlay ===
    std::uint64_t primary_last_seen_tsc{0};
    std::uint64_t dropcopy_last_seen_tsc{0};
    std::uint64_t mismatch_first_seen_tsc{0};
    std::uint64_t recon_deadline_tsc{0};

    ReconState recon_state{ReconState::Unknown};
    MismatchMask current_mismatch{};
    std::uint8_t gap_uncertainty_flags{0};

    std::uint32_t timer_generation{0};
    std::uint32_t gap_suppression_epoch{0};

    // === Divergence emission tracking ===
    std::uint64_t last_divergence_emit_tsc{0};
    MismatchMask last_emitted_mismatch{};
    std::uint32_t divergence_emit_count{0};
};

inline OrderState* create_order_state(util::Arena& arena, OrderKey key) noexcept {
    void* mem = arena.allocate(sizeof(OrderState), alignof(OrderState));
    if (!mem) {
        return nullptr;
    }
    // Placement new starts the object's lifetime and value-initialises
    // every member to its default (all zeroes / default enum values),
    // eliminating the need for a separate memset.
    auto* state = ::new (mem) OrderState{};
    state->key = key;
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

// Zero-tolerance overload delegates to the parameterized version.
[[nodiscard]] inline MismatchMask compute_mismatch(const OrderState& os) noexcept;

// Mismatch computation with tolerance parameters (conditionally instrumented when FX_PERF_ENABLED).
[[nodiscard]] inline MismatchMask compute_mismatch(
    const OrderState& os,
    std::int64_t qty_tolerance,
    std::int64_t px_tolerance
) noexcept {
    PERF_SCOPE(::util::PerfCounterId::MismatchCompute);

    assert(qty_tolerance >= 0 && "qty_tolerance must be non-negative");
    assert(px_tolerance >= 0 && "px_tolerance must be non-negative");

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

    // CumQty mismatch with tolerance (use safe_abs_diff to avoid overflow/UB)
    const auto qty_diff = safe_abs_diff(os.internal_cum_qty, os.dropcopy_cum_qty);
    if (qty_diff > static_cast<std::uint64_t>(qty_tolerance)) {
        m.set(MismatchMask::CUM_QTY);
    }

    // AvgPx mismatch with tolerance (use safe_abs_diff to avoid overflow/UB)
    const auto px_diff = safe_abs_diff(os.internal_avg_px, os.dropcopy_avg_px);
    if (px_diff > static_cast<std::uint64_t>(px_tolerance)) {
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

[[nodiscard]] inline MismatchMask compute_mismatch(const OrderState& os) noexcept {
    return compute_mismatch(os, 0, 0);
}

// Check if a divergence should be emitted or deduplicated.
// Returns true if enough time has passed since last emission with same mismatch.
// Returns false if this would be a duplicate (suppress emission).
// dedup_window_tsc is pre-computed TSC cycles (caller converts from ns once at init).
[[nodiscard]] inline bool should_emit_divergence(
    const OrderState& os,
    MismatchMask current_mismatch,
    std::uint64_t now_tsc,
    std::uint64_t dedup_window_tsc
) noexcept {
    if (current_mismatch != os.last_emitted_mismatch) {
        return true;
    }

    if (os.last_divergence_emit_tsc == 0) {
        return true;
    }

    if (now_tsc < os.last_divergence_emit_tsc) {
        return true;
    }
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

// Check if an order can be safely recycled from the hash table.
// Recycling criteria:
//   1. Reconciliation is complete (Matched or DivergedConfirmed)
//   2. Both sides have reached terminal FIX status (Filled, Canceled, or Rejected)
[[nodiscard]] inline bool is_recyclable(const OrderState& os) noexcept {
    if (os.recon_state != ReconState::Matched &&
        os.recon_state != ReconState::DivergedConfirmed) {
        return false;
    }
    return is_terminal_status(os.internal_status) &&
           is_terminal_status(os.dropcopy_status);
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

// ===== FX-7054: Gap uncertainty query functions =====
// These functions only read from OrderState and don't need SequenceTracker.
// For functions that modify both OrderState and SequenceTracker, see gap_uncertainty.hpp

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

// Clear all gap uncertainty (e.g., when order is confirmed matched)
// Note: For clearing a single source with tracker count adjustment, use gap_uncertainty.hpp
inline void clear_all_gap_uncertainty(OrderState& os) noexcept {
    os.gap_uncertainty_flags = GapUncertaintyFlags::NONE;
    // Note: gap_suppression_epoch preserved for historical tracking
}

} // namespace core
