#pragma once

#include <atomic>
#include <thread>
#include <cstdint>

#include "core/order_state_store.hpp"
#include "core/recon_config.hpp"
#include "core/recon_timer.hpp"
#include "ingest/spsc_ring.hpp"
#include "core/exec_event.hpp"
#include "core/divergence.hpp"
#include "core/sequence_tracker.hpp"
#include "util/wheel_timer.hpp"

namespace core {

using DivergenceRing = ingest::SpscRing<Divergence, 1u << 16>;
using SequenceGapRing = ingest::SpscRing<SequenceGapEvent, 1u << 16>;

struct ReconCounters {
    std::uint64_t internal_events{0};
    std::uint64_t dropcopy_events{0};

    std::uint64_t divergence_total{0};
    std::uint64_t divergence_missing_fill{0};
    std::uint64_t divergence_phantom{0};
    std::uint64_t divergence_state_mismatch{0};
    std::uint64_t divergence_quantity_mismatch{0};
    std::uint64_t divergence_timing_anomaly{0};

    std::uint64_t divergence_ring_drops{0};
    std::uint64_t store_overflow{0};

    std::uint64_t primary_seq_gaps{0};
    std::uint64_t primary_seq_duplicates{0};
    std::uint64_t primary_seq_out_of_order{0};
    std::uint64_t dropcopy_seq_gaps{0};
    std::uint64_t dropcopy_seq_duplicates{0};
    std::uint64_t dropcopy_seq_out_of_order{0};
    std::uint64_t sequence_gap_ring_drops{0};

    // ===== Two-stage reconciliation counters (FX-7053) =====
    std::uint64_t mismatch_observed{0};       // Mismatches detected, entered grace period
    std::uint64_t mismatch_confirmed{0};      // Mismatches confirmed after grace expired
    std::uint64_t false_positive_avoided{0};  // Mismatches resolved before grace expired
    std::uint64_t orders_matched{0};          // Orders that reconciled successfully (both sides agree)
    std::uint64_t divergence_deduped{0};      // Divergences suppressed (same mismatch within window)
    std::uint64_t stale_timers_skipped{0};    // Timer callbacks skipped (generation mismatch)
    std::uint64_t gap_suppressions{0};        // Divergences suppressed due to open sequence gaps
    std::uint64_t gaps_closed{0};             // FX-7054: Gap closure events (timeout or admin)
    std::uint64_t gap_timeouts{0};            // FX-7054: Gaps closed due to timeout
    std::uint64_t timer_overflow{0};          // Timer wheel bucket overflow events (FX-7053 Part 3)
    std::uint64_t divergence_resolved{0};     // Confirmed divergences that later resolved
    std::uint64_t gaps_closed_by_timeout{0};  // Sequence gaps closed due to timeout
    std::uint64_t gaps_closed_by_fill{0};     // Sequence gaps closed by out-of-order message fill
    std::uint64_t orders_recycled{0};          // Terminal orders recycled from hash table
};

// Classify divergence type based on order state and mismatch mask (FX-7053)
[[nodiscard]] inline DivergenceType classify_divergence_type(
    const OrderState& os,
    MismatchMask mismatch
) noexcept {
    if (mismatch.has(MismatchMask::EXISTENCE)) {
        if (os.seen_internal && !os.seen_dropcopy) {
            return DivergenceType::MissingDropCopy;
        } else {
            return DivergenceType::PhantomOrder;
        }
    }
    if (mismatch.has(MismatchMask::STATUS)) {
        return DivergenceType::StateMismatch;
    }
    if (mismatch.has(MismatchMask::CUM_QTY)) {
        return DivergenceType::QuantityMismatch;
    }
    // Price and execution-id mismatches are treated as state-related discrepancies.
    if (mismatch.has(MismatchMask::AVG_PX) || mismatch.has(MismatchMask::EXEC_ID)) {
        return DivergenceType::StateMismatch;
    }
    // Catch-all: any other mismatch bits not explicitly mapped above are classified
    // as state-related mismatches.
    return DivergenceType::StateMismatch;
}

class Reconciler {
public:
    Reconciler(std::atomic<bool>& stop_flag,
               ingest::SpscRing<core::ExecEvent, 1u << 16>& primary,
               ingest::SpscRing<core::ExecEvent, 1u << 16>& dropcopy,
               OrderStateStore& store,
               ReconCounters& counters,
               DivergenceRing& divergence_ring,
               SequenceGapRing& seq_gap_ring,
               util::WheelTimer* timer_wheel = nullptr,
               const ReconConfig& config = default_recon_config()) noexcept;

    void run();
    void process_event_for_test(const ExecEvent& ev) noexcept { process_event(ev); }

    // ===== Two-stage pipeline helpers (FX-7053) =====

    // Check if both primary and dropcopy have been seen for an order
    [[nodiscard]] static bool both_sides_seen(const OrderState& os) noexcept {
        return os.seen_internal && os.seen_dropcopy;
    }

    // Check if divergence should be suppressed due to sequence gap
    // Note: non-const because it may close timed-out gaps
    [[nodiscard]] bool is_gap_suppressed(const OrderState& os) noexcept;

    // Enter grace period for an order with detected mismatch
    void enter_grace_period(OrderState& os, MismatchMask mismatch, std::uint64_t now_tsc) noexcept;

    // Exit grace period (mismatch resolved before deadline)
    void exit_grace_period(OrderState& os, std::uint64_t now_tsc) noexcept;

    // Handle timer expiration callback from wheel (now_tsc injected for testability)
    void on_grace_deadline_expired(OrderKey key, std::uint32_t scheduled_gen,
                                   std::uint64_t now_tsc) noexcept;

    // Emit confirmed divergence (with deduplication check)
    // Uses the inline function should_emit_divergence() from order_state.hpp
    void emit_confirmed_divergence(OrderState& os,
                                   MismatchMask mismatch,
                                   std::uint64_t now_tsc) noexcept;

    // Handle state transition based on mismatch (FX-7053 Part 3)
    void handle_recon_state_transition(OrderState& os, MismatchMask new_mismatch,
                                       std::uint64_t now_tsc) noexcept;

    [[nodiscard]] std::uint64_t last_poll_tsc() const noexcept { return last_poll_tsc_; }
    void set_last_poll_tsc_for_test(std::uint64_t tsc) noexcept { last_poll_tsc_ = tsc; }

    // FX-7054: Administrative gap closure (for testing and manual intervention)
    void close_session_gap(Source source) noexcept;

private:
    void process_event(const ExecEvent& ev) noexcept;
    void increment_divergence_counter(DivergenceType type) noexcept;
    
    void check_gap_timeouts(std::uint64_t now_tsc) noexcept;

    std::atomic<bool>& stop_flag_;
    ingest::SpscRing<core::ExecEvent, 1u << 16>& primary_;
    ingest::SpscRing<core::ExecEvent, 1u << 16>& dropcopy_;
    OrderStateStore& store_;
    ReconCounters& counters_;
    DivergenceRing& divergence_ring_;
    SequenceGapRing& seq_gap_ring_;

    SequenceTracker primary_seq_tracker_{};
    SequenceTracker dropcopy_seq_tracker_{};

    util::WheelTimer* timer_wheel_{nullptr};
    ReconConfig config_{};
    std::uint64_t last_poll_tsc_{0};

    // Pre-computed TSC conversions of immutable config values (avoid ns_to_tsc on hot path)
    std::uint64_t grace_period_tsc_{0};
    std::uint64_t gap_timeout_tsc_{0};
    std::uint64_t gap_recheck_period_tsc_{0};
    std::uint64_t dedup_window_tsc_{0};
};

} // namespace core
