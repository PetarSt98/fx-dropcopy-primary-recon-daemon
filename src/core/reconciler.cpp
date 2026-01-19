#include "core/reconciler.hpp"

#include <chrono>
#include <thread>

#include "core/order_state.hpp"
#include "core/order_lifecycle.hpp"
#include "util/async_log.hpp"

namespace core {

Reconciler::Reconciler(std::atomic<bool>& stop_flag,
                       ingest::SpscRing<core::ExecEvent, 1u << 16>& primary,
                       ingest::SpscRing<core::ExecEvent, 1u << 16>& dropcopy,
                       OrderStateStore& store,
                       ReconCounters& counters,
                       DivergenceRing& divergence_ring,
                       SequenceGapRing& seq_gap_ring) noexcept
    : stop_flag_(stop_flag),
      primary_(primary),
      dropcopy_(dropcopy),
      store_(store),
      counters_(counters),
      divergence_ring_(divergence_ring),
      seq_gap_ring_(seq_gap_ring) {}

// New constructor with timer wheel and config (FX-7053)
Reconciler::Reconciler(std::atomic<bool>& stop_flag,
                       ingest::SpscRing<core::ExecEvent, 1u << 16>& primary,
                       ingest::SpscRing<core::ExecEvent, 1u << 16>& dropcopy,
                       OrderStateStore& store,
                       ReconCounters& counters,
                       DivergenceRing& divergence_ring,
                       SequenceGapRing& seq_gap_ring,
                       util::WheelTimer* timer_wheel,
                       const ReconConfig& config) noexcept
    : stop_flag_(stop_flag),
      primary_(primary),
      dropcopy_(dropcopy),
      store_(store),
      counters_(counters),
      divergence_ring_(divergence_ring),
      seq_gap_ring_(seq_gap_ring),
      timer_wheel_(timer_wheel),
      config_(config) {}

void Reconciler::increment_divergence_counter(DivergenceType type) noexcept {
    switch (type) {
    case DivergenceType::MissingFill:
        ++counters_.divergence_missing_fill;
        break;
    case DivergenceType::MissingDropCopy:
        // No dedicated counter yet (could be added in future)
        break;
    case DivergenceType::PhantomOrder:
        ++counters_.divergence_phantom;
        break;
    case DivergenceType::StateMismatch:
        ++counters_.divergence_state_mismatch;
        break;
    case DivergenceType::QuantityMismatch:
        ++counters_.divergence_quantity_mismatch;
        break;
    case DivergenceType::TimingAnomaly:
        ++counters_.divergence_timing_anomaly;
        break;
    }
}

void Reconciler::process_event(const ExecEvent& ev) noexcept {
    SequenceGapEvent gap_ev{};
    SequenceGapEvent* gap_ptr = &gap_ev;
    const std::uint64_t now_ts = ev.ingest_tsc;

    bool has_gap = false;
    if (ev.source == Source::Primary) {
        has_gap = track_sequence(primary_seq_tracker_, ev.source, ev.session_id, ev.seq_num, now_ts, gap_ptr);
    } else if (ev.source == Source::DropCopy) {
        has_gap = track_sequence(dropcopy_seq_tracker_, ev.source, ev.session_id, ev.seq_num, now_ts, gap_ptr);
    }

    if (has_gap) {
        switch (gap_ev.source) {
        case Source::Primary:
            if (gap_ev.kind == GapKind::Gap) {
                ++counters_.primary_seq_gaps;
            } else if (gap_ev.kind == GapKind::Duplicate) {
                ++counters_.primary_seq_duplicates;
            } else {
                ++counters_.primary_seq_out_of_order;
            }
            break;
        case Source::DropCopy:
            if (gap_ev.kind == GapKind::Gap) {
                ++counters_.dropcopy_seq_gaps;
            } else if (gap_ev.kind == GapKind::Duplicate) {
                ++counters_.dropcopy_seq_duplicates;
            } else {
                ++counters_.dropcopy_seq_out_of_order;
            }
            break;
        }

        if (!seq_gap_ring_.try_push(gap_ev)) {
            ++counters_.sequence_gap_ring_drops;
            LOG_HOT_LVL(::util::LogLevel::Warn, "RECON",
                        "seq_gap_ring_drop src=%u session=%u expected=%llu seen=%llu kind=%u",
                        static_cast<unsigned>(gap_ev.source), gap_ev.session_id,
                        static_cast<unsigned long long>(gap_ev.expected_seq),
                        static_cast<unsigned long long>(gap_ev.seen_seq),
                        static_cast<unsigned>(gap_ev.kind));
        }
    }

    OrderState* st = store_.upsert(ev);
    if (!st) {
        ++counters_.store_overflow;
        LOG_HOT_LVL(::util::LogLevel::Warn, "RECON",
                    "store_overflow src=%u session=%u seq=%llu",
                    static_cast<unsigned>(ev.source), ev.session_id,
                    static_cast<unsigned long long>(ev.seq_num));
        return;
    }

    if (primary_seq_tracker_.gap_open || dropcopy_seq_tracker_.gap_open) {
        st->has_gap = true;
    }

    bool ok = false;
    if (ev.source == Source::Primary) {
        ++counters_.internal_events;
        ok = apply_internal_exec(*st, ev);
    } else {
        ++counters_.dropcopy_events;
        ok = apply_dropcopy_exec(*st, ev);
    }

    if (!ok) {
        Divergence div{};
        fill_divergence_snapshot(*st, DivergenceType::StateMismatch, div);
        if (!divergence_ring_.try_push(div)) {
            ++counters_.divergence_ring_drops;
        } else {
            ++counters_.divergence_total;
            increment_divergence_counter(div.type);
        }
        return;
    }

    Divergence div{};
    if (classify_divergence(*st, div, qty_tolerance_, px_tolerance_, timing_slack_)) {
        if (!divergence_ring_.try_push(div)) {
            ++counters_.divergence_ring_drops;
            LOG_HOT_LVL(::util::LogLevel::Warn, "RECON",
                        "divergence_ring_drop type=%u key=%llu",
                        static_cast<unsigned>(div.type),
                        static_cast<unsigned long long>(div.key));
            return;
        }
        ++counters_.divergence_total;
        increment_divergence_counter(div.type);
    }
}

void Reconciler::run() {
    ExecEvent primary_evt{};
    ExecEvent dropcopy_evt{};
    std::uint32_t backoff = 0;
    while (!stop_flag_.load(std::memory_order_acquire)) {
        bool consumed = false;
        if (primary_.try_pop(primary_evt)) {
            process_event(primary_evt);
            consumed = true;
        }
        if (dropcopy_.try_pop(dropcopy_evt)) {
            process_event(dropcopy_evt);
            consumed = true;
        }
        if (!consumed) {
            if (backoff < 16) {
                ++backoff;
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        } else {
            backoff = 0;
        }
    }
}

// ===== Two-stage pipeline helper implementations (FX-7053) =====

bool Reconciler::is_gap_suppressed(const OrderState& os) const noexcept {
    if (!config_.enable_gap_suppression) {
        return false;
    }

    // Order is suppressed if it has been flagged during a gap
    // and we still have an open gap on either side
    if (os.gap_suppression_epoch == 0) {
        return false;
    }

    return primary_seq_tracker_.gap_open || dropcopy_seq_tracker_.gap_open;
}

void Reconciler::enter_grace_period(OrderState& os, MismatchMask mismatch,
                                    std::uint64_t now_tsc) noexcept {
    os.recon_state = ReconState::InGrace;
    os.current_mismatch = mismatch;
    os.mismatch_first_seen_tsc = now_tsc;

    if (timer_wheel_) {
        const std::uint64_t deadline = now_tsc + config_.grace_period_ns;
        schedule_recon_deadline(*timer_wheel_, os, deadline);
    }

    ++counters_.mismatch_observed;
}

void Reconciler::exit_grace_period(OrderState& os, std::uint64_t /*now_tsc*/) noexcept {
    // Cancel timer by incrementing generation
    cancel_recon_deadline(os);

    os.recon_state = ReconState::Matched;
    os.current_mismatch = MismatchMask{};

    ++counters_.false_positive_avoided;
    ++counters_.orders_matched;
}

void Reconciler::on_grace_deadline_expired(OrderKey key, std::uint32_t scheduled_gen) noexcept {
    OrderState* os = store_.find(key);
    if (!os) {
        return;  // Order was recycled
    }

    if (!is_timer_valid(*os, scheduled_gen)) {
        ++counters_.stale_timers_skipped;
        return;
    }

    // Re-check mismatch at expiration time
    const MismatchMask mismatch = compute_mismatch(*os);
    const std::uint64_t now = last_poll_tsc_;  // Use last known time

    if (mismatch.none()) {
        // Mismatch resolved - false positive avoided
        os->recon_state = ReconState::Matched;
        ++counters_.false_positive_avoided;
        ++counters_.orders_matched;
    } else if (is_gap_suppressed(*os)) {
        // Gap still open - suppress and reschedule
        os->recon_state = ReconState::SuppressedByGap;
        if (timer_wheel_) {
            refresh_recon_deadline(*timer_wheel_, *os, now + config_.gap_recheck_period_ns);
        }
        ++counters_.gap_suppressions;
    } else {
        // Confirmed divergence
        os->recon_state = ReconState::DivergedConfirmed;
        emit_confirmed_divergence(*os, mismatch, now);
        ++counters_.mismatch_confirmed;
    }
}

void Reconciler::emit_confirmed_divergence(OrderState& os, MismatchMask mismatch,
                                           std::uint64_t now_tsc) noexcept {
    // Check deduplication using free function from order_state.hpp
    if (!should_emit_divergence(os, mismatch, now_tsc, config_.divergence_dedup_window_ns)) {
        ++counters_.divergence_deduped;
        return;
    }

    // Build and emit divergence event
    Divergence div{};
    div.key = os.key;
    div.type = classify_divergence_type(os, mismatch);
    div.internal_status = os.internal_status;
    div.dropcopy_status = os.dropcopy_status;
    div.internal_cum_qty = os.internal_cum_qty;
    div.dropcopy_cum_qty = os.dropcopy_cum_qty;
    div.internal_avg_px = os.internal_avg_px;
    div.dropcopy_avg_px = os.dropcopy_avg_px;
    div.internal_ts = os.last_internal_ts;
    div.dropcopy_ts = os.last_dropcopy_ts;
    div.detect_tsc = now_tsc;
    div.mismatch_mask = mismatch.bits();

    if (!divergence_ring_.try_push(div)) {
        ++counters_.divergence_ring_drops;
    }

    // Record emission for deduplication
    record_divergence_emission(os, mismatch, now_tsc);
}

} // namespace core
