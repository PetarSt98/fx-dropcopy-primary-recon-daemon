#pragma once

#include <cstdint>
#include <cstdlib>

#include "core/order_state.hpp"

namespace core {

enum class DivergenceType : std::uint8_t {
    MissingFill,      // DropCopy filled/partial, internal not reflecting it
    PhantomOrder,     // DropCopy has order, internal has no record
    StateMismatch,    // OrdStatus mismatch (e.g. Filled vs Working)
    QuantityMismatch, // CumQty/AvgPx mismatch beyond tolerance
    TimingAnomaly,    // DropCopy significantly earlier than internal
    MissingDropCopy   // Internal seen but no dropcopy (FX-7053)
};

struct Divergence {
    OrderKey key{0};
    DivergenceType type{DivergenceType::StateMismatch};
    OrdStatus internal_status{OrdStatus::Unknown};
    OrdStatus dropcopy_status{OrdStatus::Unknown};
    std::int64_t internal_cum_qty{0};
    std::int64_t dropcopy_cum_qty{0};
    std::int64_t internal_avg_px{0};
    std::int64_t dropcopy_avg_px{0};
    std::uint64_t internal_ts{0};
    std::uint64_t dropcopy_ts{0};
    std::uint64_t detect_tsc{0};      // TSC when divergence was detected (FX-7053)
    std::uint8_t mismatch_mask{0};    // MismatchMask bits at detection time (FX-7053)
};

inline void fill_divergence_snapshot(const OrderState& state,
                                     DivergenceType type,
                                     Divergence& out) noexcept {
    out.key = state.key;
    out.type = type;
    out.internal_status = state.internal_status;
    out.dropcopy_status = state.dropcopy_status;
    out.internal_cum_qty = state.internal_cum_qty;
    out.dropcopy_cum_qty = state.dropcopy_cum_qty;
    out.internal_avg_px = state.internal_avg_px;
    out.dropcopy_avg_px = state.dropcopy_avg_px;
    out.internal_ts = state.last_internal_ts;
    out.dropcopy_ts = state.last_dropcopy_ts;
}

// Priority order: PhantomOrder > MissingFill > StateMismatch > QuantityMismatch > TimingAnomaly.
inline bool classify_divergence(const OrderState& state,
                                Divergence& out,
                                std::int64_t qty_tolerance = 0,
                                std::int64_t px_tolerance = 0,
                                std::uint64_t timing_slack = 0) noexcept {
    using OS = OrdStatus;

    if (!state.seen_dropcopy) {
        return false;
    }

    if (!state.seen_internal) {
        fill_divergence_snapshot(state, DivergenceType::PhantomOrder, out);
        return true;
    }

    const bool dropcopy_is_fill = state.dropcopy_status == OS::Filled ||
                                  state.dropcopy_status == OS::PartiallyFilled;
    const bool internal_pre_filled = state.internal_status == OS::New ||
                                     state.internal_status == OS::PendingNew ||
                                     state.internal_status == OS::Working;

    if (dropcopy_is_fill && internal_pre_filled) {
        fill_divergence_snapshot(state, DivergenceType::MissingFill, out);
        return true;
    }

    if (state.dropcopy_status != state.internal_status) {
        fill_divergence_snapshot(state, DivergenceType::StateMismatch, out);
        return true;
    }

    const auto qty_diff = std::llabs(state.dropcopy_cum_qty - state.internal_cum_qty);
    const auto px_diff = std::llabs(state.dropcopy_avg_px - state.internal_avg_px);
    if (qty_diff > qty_tolerance || px_diff > px_tolerance) {
        fill_divergence_snapshot(state, DivergenceType::QuantityMismatch, out);
        return true;
    }

    if (state.last_dropcopy_ts + timing_slack < state.last_internal_ts) {
        fill_divergence_snapshot(state, DivergenceType::TimingAnomaly, out);
        return true;
    }

    return false;
}

} // namespace core

