#pragma once

#include <cstdint>

#include "core/order_state.hpp"
#include "core/recon_state.hpp"
#include "util/wheel_timer.hpp"

namespace core {

// Timer operation helpers for reconciliation grace period management.
// These implement the generation-based lazy cancellation pattern.
//
// Pattern overview:
// - Each OrderState has a timer_generation counter
// - When scheduling, we store (key, current_generation) in the wheel
// - To "cancel", we just increment timer_generation (O(1), no wheel lookup)
// - On expiry callback, we compare scheduled generation vs current generation
// - If mismatch, the timer was cancelled - skip processing
//
// This avoids:
// - O(N) scans to find and remove cancelled timers
// - Complex data structures for timer handles
// - Memory management for timer entries

// Schedule a grace period deadline for an order.
// Returns true if successfully scheduled, false if wheel bucket overflowed.
[[nodiscard]] inline bool schedule_recon_deadline(
    util::WheelTimer& wheel,
    OrderState& os,
    std::uint64_t deadline_tsc
) noexcept {
    // Increment generation to invalidate any previously scheduled timer
    ++os.timer_generation;

    // Store the deadline in the order state for reference
    os.recon_deadline_tsc = deadline_tsc;

    // Schedule with current generation
    return wheel.schedule(os.key, os.timer_generation, deadline_tsc);
}

// "Cancel" a timer by incrementing the generation.
// The scheduled entry becomes stale and will be skipped on expiry.
// This is O(1) - no wheel lookup required.
inline void cancel_recon_deadline(OrderState& os) noexcept {
    ++os.timer_generation;
    os.recon_deadline_tsc = 0;
}

// Refresh/reschedule a timer with a new deadline.
// Equivalent to cancel + schedule, but slightly more efficient.
[[nodiscard]] inline bool refresh_recon_deadline(
    util::WheelTimer& wheel,
    OrderState& os,
    std::uint64_t new_deadline_tsc
) noexcept {
    // Just schedule - this implicitly cancels old timer via generation increment
    return schedule_recon_deadline(wheel, os, new_deadline_tsc);
}

// Check if a timer callback should be processed or skipped.
// Call this at the start of the expiry callback.
// Returns true if the timer is still valid (should process).
// Returns false if the timer was cancelled (skip processing).
[[nodiscard]] inline bool is_timer_valid(
    const OrderState& os,
    std::uint32_t scheduled_generation
) noexcept {
    return os.timer_generation == scheduled_generation;
}

// ============================================================================
// Usage Example (for documentation - shows reconciler integration pattern)
// ============================================================================
//
// SCENARIO 1: Entering grace period (mismatch detected, both sides seen)
//
//     void Reconciler::on_exec_event(const ExecEvent& ev) {
//         OrderState* os = store_.upsert(ev);
//         apply_exec(*os, ev);
//
//         if (os->seen_internal && os->seen_dropcopy) {
//             MismatchMask mismatch = compute_mismatch(*os);
//
//             if (mismatch.any() && os->recon_state != ReconState::InGrace) {
//                 // Mismatch detected - enter grace period
//                 os->recon_state = ReconState::InGrace;
//                 os->current_mismatch = mismatch;
//                 os->mismatch_first_seen_tsc = ev.ingest_tsc;
//
//                 uint64_t deadline = ev.ingest_tsc + grace_period_ns_;
//                 schedule_recon_deadline(timer_wheel_, *os, deadline);
//             }
//             else if (mismatch.none() && os->recon_state == ReconState::InGrace) {
//                 // Mismatch resolved before deadline - cancel timer
//                 cancel_recon_deadline(*os);
//                 os->recon_state = ReconState::Matched;
//                 ++counters_.false_positive_avoided;
//             }
//         }
//     }
//
// SCENARIO 2: Processing timer expiry in main loop
//
//     void Reconciler::run() {
//         while (!stop_flag_) {
//             // ... poll exec events ...
//
//             // Poll timer wheel for expired deadlines
//             uint64_t now = rdtsc_ns();
//             timer_wheel_.poll_expired(now, [this](OrderKey key, uint32_t gen) {
//                 on_deadline_expired(key, gen);
//             });
//         }
//     }
//
// SCENARIO 3: Handling timer expiry
//
//     void Reconciler::on_deadline_expired(OrderKey key, uint32_t scheduled_gen) {
//         OrderState* os = store_.find(key);
//         if (!os) return;  // Order was recycled
//
//         if (!is_timer_valid(*os, scheduled_gen)) {
//             // Timer was cancelled (generation mismatch) - skip
//             ++counters_.stale_timers_skipped;
//             return;
//         }
//
//         // Timer is valid - check current state
//         MismatchMask mismatch = compute_mismatch(*os);
//
//         if (mismatch.none()) {
//             // Mismatch resolved (late message arrived) - false positive avoided
//             os->recon_state = ReconState::Matched;
//             ++counters_.false_positive_avoided;
//         }
//         else if (os->has_gap) {
//             // Sequence gap open - suppress divergence, reschedule
//             os->recon_state = ReconState::SuppressedByGap;
//             refresh_recon_deadline(timer_wheel_, *os, now_tsc() + gap_recheck_ns_);
//         }
//         else {
//             // Confirmed divergence - emit alert
//             os->recon_state = ReconState::DivergedConfirmed;
//             emit_divergence(*os, mismatch);
//         }
//     }
//
// ============================================================================

} // namespace core
