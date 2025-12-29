#include "core/reconciler.hpp"

#include <chrono>
#include <thread>

#include "core/order_state.hpp"
#include "core/order_lifecycle.hpp"
#include "util/hot_log.hpp"

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

void Reconciler::increment_divergence_counter(DivergenceType type) noexcept {
    switch (type) {
    case DivergenceType::MissingFill:
        ++counters_.divergence_missing_fill;
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
            HOT_RING_DROP(1u, static_cast<std::uint32_t>(gap_ev.kind), 1u);
        }
    }

    OrderState* st = store_.upsert(ev);
    if (!st) {
        ++counters_.store_overflow;
        HOT_STORE_OVERFLOW(static_cast<std::uint32_t>(ev.source),
                           static_cast<std::uint32_t>(store_.bucket_count()),
                           static_cast<std::uint32_t>(store_.size()));
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
            HOT_RING_DROP(2u, static_cast<std::uint32_t>(div.type), 1u);
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

} // namespace core
