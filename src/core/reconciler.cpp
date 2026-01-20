#include "core/reconciler.hpp"

#include <thread>

#include "core/order_state.hpp"
#include "core/order_lifecycle.hpp"
#include "core/gap_uncertainty.hpp"
#include "util/async_log.hpp"
#include "util/rdtsc.hpp"
#include "util/tsc_calibration.hpp"

// CPU pause intrinsics for HFT busy-wait loops
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    #ifdef _MSC_VER
        #include <intrin.h>
        #define CPU_PAUSE() _mm_pause()
    #else
        #define CPU_PAUSE() __builtin_ia32_pause()
    #endif
#else
    // Fallback for non-x86 architectures (ARM, etc.)
    #define CPU_PAUSE() ((void)0)
#endif

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
    // === Sequence tracking (unchanged) ===
    SequenceGapEvent gap_ev{};
    SequenceGapEvent* gap_ptr = &gap_ev;
    const std::uint64_t now_tsc = ev.ingest_tsc;  // Use event timestamp for determinism

    bool has_gap = false;
    if (ev.source == Source::Primary) {
        has_gap = track_sequence(primary_seq_tracker_, ev.source, ev.session_id, ev.seq_num, now_tsc, gap_ptr);
    } else if (ev.source == Source::DropCopy) {
        has_gap = track_sequence(dropcopy_seq_tracker_, ev.source, ev.session_id, ev.seq_num, now_tsc, gap_ptr);
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

    // === Get/create order state ===
    OrderState* st = store_.upsert(ev);
    if (!st) {
        ++counters_.store_overflow;
        LOG_HOT_LVL(::util::LogLevel::Warn, "RECON",
                    "store_overflow src=%u session=%u seq=%llu",
                    static_cast<unsigned>(ev.source), ev.session_id,
                    static_cast<unsigned long long>(ev.seq_num));
        return;
    }

    // FX-7054: Mark orders affected by open gaps using per-session epoch tracking
    // Note: mark_gap_uncertainty() internally increments orders_in_gap_count
    if (ev.source == Source::Primary && primary_seq_tracker_.gap_open) {
        mark_gap_uncertainty(*st, Source::Primary, primary_seq_tracker_);
    }
    if (ev.source == Source::DropCopy && dropcopy_seq_tracker_.gap_open) {
        mark_gap_uncertainty(*st, Source::DropCopy, dropcopy_seq_tracker_);
    }

    // === Update appropriate view ===
    bool ok = false;
    if (ev.source == Source::Primary) {
        ++counters_.internal_events;
        ok = apply_internal_exec(*st, ev);
        st->primary_last_seen_tsc = now_tsc;
    } else {
        ++counters_.dropcopy_events;
        ok = apply_dropcopy_exec(*st, ev);
        st->dropcopy_last_seen_tsc = now_tsc;
    }

    // Handle invalid state transitions (emit immediately - this is an error)
    if (!ok) {
        MismatchMask error_mismatch{};
        error_mismatch.set(MismatchMask::STATUS);
        st->recon_state = ReconState::DivergedConfirmed;
        emit_confirmed_divergence(*st, error_mismatch, now_tsc);
        ++counters_.mismatch_confirmed;
        return;
    }

    // === Two-stage reconciliation ===
    if (config_.enable_windowed_recon && timer_wheel_) {
        // Compute current mismatch BEFORE state transition
        const MismatchMask new_mismatch = compute_mismatch(*st, config_.qty_tolerance,
                                                            config_.px_tolerance);
        st->current_mismatch = new_mismatch;  // Set BEFORE transition

        // Handle state transition based on mismatch
        handle_recon_state_transition(*st, new_mismatch, now_tsc);
    } else {
        // Legacy behavior: immediate emission (backward compatibility / testing)
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
}

void Reconciler::run() {
    ExecEvent primary_evt{};
    ExecEvent dropcopy_evt{};
    std::uint32_t backoff = 0;
    last_poll_tsc_ = util::rdtsc();
    
    // FX-7054: Gap timeout tracking - check once per second (not in hot path)
    static constexpr std::uint64_t GAP_CHECK_INTERVAL_NS = 1'000'000'000ULL;  // 1 second
    std::uint64_t last_gap_check_tsc = util::rdtsc();
    const std::uint64_t gap_check_interval_tsc = util::ns_to_tsc(GAP_CHECK_INTERVAL_NS);

    while (!stop_flag_.load(std::memory_order_acquire)) {
        bool consumed = false;

        // Hot path: drain event queues
        if (primary_.try_pop(primary_evt)) {
            process_event(primary_evt);
            last_poll_tsc_ = primary_evt.ingest_tsc;
            consumed = true;
        }
        if (dropcopy_.try_pop(dropcopy_evt)) {
            process_event(dropcopy_evt);
            last_poll_tsc_ = std::max(last_poll_tsc_, dropcopy_evt.ingest_tsc);
            consumed = true;
        }

        // Warm path: poll timer wheel for expired deadlines
        // Always use fresh rdtsc() to avoid missing timer deadlines
        // (using stale last_poll_tsc_ could cause timers to be skipped)
        const std::uint64_t now = util::rdtsc();
        if (timer_wheel_) {
            timer_wheel_->poll_expired(now, [this](OrderKey key, std::uint32_t gen) {
                on_grace_deadline_expired(key, gen);
            });
        }
        
        // FX-7054: Periodic gap timeout check (not in hot path - once per second)
        if (now - last_gap_check_tsc > gap_check_interval_tsc) {
            check_gap_timeouts(now);
            last_gap_check_tsc = now;
        }

        // Backoff when idle - exponential backoff reduces CPU burn
        if (!consumed) {
            if (backoff == 0) {
                backoff = 1;
            } else if (backoff < 256) {
                backoff <<= 1;  // Exponential: 1,2,4,8,16,32,64,128,256
            }
            // Execute pause instructions proportional to backoff level
            // This reduces CPU power while maintaining low-latency wake-up
            for (std::uint32_t i = 0; i < backoff; ++i) {
                CPU_PAUSE();
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

    // FX-7054: Use Part 1's is_suppressed_by_gap() helper for each source
    // Returns true if order's epoch matches tracker's current open gap epoch
    if (is_suppressed_by_gap(os, Source::Primary, primary_seq_tracker_)) {
        return true;
    }
    
    if (is_suppressed_by_gap(os, Source::DropCopy, dropcopy_seq_tracker_)) {
        return true;
    }

    return false;
}

void Reconciler::enter_grace_period(OrderState& os, MismatchMask mismatch,
                                    std::uint64_t now_tsc) noexcept {
    os.recon_state = ReconState::InGrace;
    os.current_mismatch = mismatch;
    os.mismatch_first_seen_tsc = now_tsc;
    // Convert nanoseconds config to TSC cycles before adding to TSC timestamp
    os.recon_deadline_tsc = now_tsc + util::ns_to_tsc(config_.grace_period_ns);

    // Schedule timer (requires non-null timer_wheel_)
    if (timer_wheel_) {
        const bool scheduled = schedule_recon_deadline(*timer_wheel_, os, os.recon_deadline_tsc);
        if (!scheduled) {
            // Timer wheel bucket overflow - fallback to immediate emission
            // This is degraded mode, should be monitored
            ++counters_.timer_overflow;
            os.recon_state = ReconState::DivergedConfirmed;
            emit_confirmed_divergence(os, mismatch, now_tsc);
            ++counters_.mismatch_confirmed;
            return;
        }
    }

    ++counters_.mismatch_observed;
}

void Reconciler::exit_grace_period(OrderState& os, std::uint64_t /*now_tsc*/) noexcept {
    // Cancel timer by incrementing generation
    cancel_recon_deadline(os);

    os.recon_state = ReconState::Matched;
    os.current_mismatch = MismatchMask{};
    
    // FX-7054: Clear gap uncertainty when order matches
    clear_all_gap_uncertainty(os);

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
            // Convert nanoseconds config to TSC cycles before adding to TSC timestamp
            const bool rescheduled = refresh_recon_deadline(*timer_wheel_, *os, now + util::ns_to_tsc(config_.gap_recheck_period_ns));
            if (!rescheduled) {
                // Timer overflow during gap recheck - emit divergence
                ++counters_.timer_overflow;
                os->recon_state = ReconState::DivergedConfirmed;
                emit_confirmed_divergence(*os, mismatch, now);
                ++counters_.mismatch_confirmed;
                return;
            }
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
    
    // FX-7054: Clear gap uncertainty after confirmed divergence
    clear_all_gap_uncertainty(os);
}

void Reconciler::handle_recon_state_transition(
    OrderState& os,
    MismatchMask new_mismatch,
    std::uint64_t now_tsc
) noexcept {
    switch (os.recon_state) {
        case ReconState::Unknown:
            // First event - determine initial state
            if (os.seen_internal && !os.seen_dropcopy) {
                os.recon_state = ReconState::AwaitingDropCopy;
            } else if (os.seen_dropcopy && !os.seen_internal) {
                os.recon_state = ReconState::AwaitingPrimary;
            } else if (both_sides_seen(os)) {
                // Both sides seen on first event (unusual but handle it)
                if (new_mismatch.any()) {
                    enter_grace_period(os, new_mismatch, now_tsc);
                } else {
                    os.recon_state = ReconState::Matched;
                    ++counters_.orders_matched;
                }
            }
            break;

        case ReconState::AwaitingPrimary:
            if (os.seen_internal) {
                // Primary arrived - now we can compare
                if (new_mismatch.any()) {
                    enter_grace_period(os, new_mismatch, now_tsc);
                } else {
                    os.recon_state = ReconState::Matched;
                    ++counters_.orders_matched;
                }
            }
            break;

        case ReconState::AwaitingDropCopy:
            if (os.seen_dropcopy) {
                // DropCopy arrived - now we can compare
                if (new_mismatch.any()) {
                    enter_grace_period(os, new_mismatch, now_tsc);
                } else {
                    os.recon_state = ReconState::Matched;
                    ++counters_.orders_matched;
                }
            }
            break;

        case ReconState::InGrace:
            if (new_mismatch.none()) {
                // Mismatch resolved before deadline - false positive avoided!
                exit_grace_period(os, now_tsc);
            } else {
                // Still mismatched: update tracked mismatch if its type/contents changed.
                if (new_mismatch != os.current_mismatch) {
                    os.current_mismatch = new_mismatch;
                }
                // Timer started when entering grace will handle confirmation.
            }
            break;

        case ReconState::Matched:
            if (new_mismatch.any()) {
                // New mismatch after previous match - re-enter grace
                enter_grace_period(os, new_mismatch, now_tsc);
            }
            break;

        case ReconState::DivergedConfirmed:
            if (new_mismatch.none()) {
                // Divergence resolved - return to matched
                os.recon_state = ReconState::Matched;
                ++counters_.divergence_resolved;
            }
            break;

        case ReconState::SuppressedByGap:
            if (!is_gap_suppressed(os)) {
                // Gap closed - re-evaluate
                if (new_mismatch.any()) {
                    enter_grace_period(os, new_mismatch, now_tsc);
                } else {
                    os.recon_state = ReconState::Matched;
                    ++counters_.orders_matched;
                }
            }
            break;
    }
}

// ===== FX-7054: Gap management implementations =====

void Reconciler::close_session_gap(Source source) noexcept {
    SequenceTracker& tracker = (source == Source::Primary)
        ? primary_seq_tracker_
        : dropcopy_seq_tracker_;
    
    if (!tracker.gap_open) {
        return;  // Gap already closed
    }
    
    // Capture values for logging before closing
    const auto epoch = tracker.gap_epoch;
    const auto orders_affected = tracker.orders_in_gap_count;
    const auto gap_opened_tsc = tracker.gap_opened_tsc;
    
    // Use Part 1's close_gap() helper
    const bool was_closed = close_gap(tracker);
    
    if (was_closed) {
        ++counters_.gaps_closed;
        
        // Log gap closure (gaps are rare, OK to log)
        LOG_HOT_LVL(::util::LogLevel::Info, "RECON",
                    "gap_closed src=%u epoch=%u orders_affected=%u duration_tsc=%llu",
                    static_cast<unsigned>(source),
                    static_cast<unsigned>(epoch),
                    static_cast<unsigned>(orders_affected),
                    static_cast<unsigned long long>(util::rdtsc() - gap_opened_tsc));
    }
    
    // Orders in SuppressedByGap state will be re-evaluated when:
    // 1. Their recheck timer fires (already scheduled in on_grace_deadline_expired)
    // 2. They receive a new event (triggers handle_recon_state_transition)
    //
    // No O(N) scan needed - lazy re-evaluation via existing mechanisms
}

void Reconciler::check_gap_timeouts(std::uint64_t now_tsc) noexcept {
    const std::uint64_t gap_timeout_tsc = util::ns_to_tsc(config_.gap_timeout_ns);
    
    // Check Primary gap timeout
    // Note: gap_opened_tsc is always set when gap_open becomes true (see track_sequence),
    // but we check > 0 as a defensive measure against uninitialized state
    if (primary_seq_tracker_.gap_open &&
        primary_seq_tracker_.gap_opened_tsc != 0 &&
        (now_tsc - primary_seq_tracker_.gap_opened_tsc) > gap_timeout_tsc) {
        
        LOG_HOT_LVL(::util::LogLevel::Warn, "RECON",
                    "gap_timeout src=Primary epoch=%u missing=[%llu,%llu]",
                    static_cast<unsigned>(primary_seq_tracker_.gap_epoch),
                    static_cast<unsigned long long>(primary_seq_tracker_.gap_start_seq),
                    static_cast<unsigned long long>(primary_seq_tracker_.gap_last_missing_seq));
        
        close_session_gap(Source::Primary);
        ++counters_.gap_timeouts;
    }
    
    // Check DropCopy gap timeout
    if (dropcopy_seq_tracker_.gap_open &&
        dropcopy_seq_tracker_.gap_opened_tsc != 0 &&
        (now_tsc - dropcopy_seq_tracker_.gap_opened_tsc) > gap_timeout_tsc) {
        
        LOG_HOT_LVL(::util::LogLevel::Warn, "RECON",
                    "gap_timeout src=DropCopy epoch=%u missing=[%llu,%llu]",
                    static_cast<unsigned>(dropcopy_seq_tracker_.gap_epoch),
                    static_cast<unsigned long long>(dropcopy_seq_tracker_.gap_start_seq),
                    static_cast<unsigned long long>(dropcopy_seq_tracker_.gap_last_missing_seq));
        
        close_session_gap(Source::DropCopy);
        ++counters_.gap_timeouts;
    }
}

} // namespace core
