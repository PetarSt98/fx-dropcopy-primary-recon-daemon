#pragma once

#include <atomic>
#include <thread>
#include <cstdint>
#include <cstddef>

#include "core/order_state_store.hpp"
#include "ingest/spsc_ring.hpp"
#include "core/exec_event.hpp"
#include "core/divergence.hpp"
#include "core/sequence_tracker.hpp"
#include "persist/audit_log_format.hpp"

namespace core {

template <std::size_t Capacity = 4096>
using DivergenceRingT = ingest::SpscRing<Divergence, Capacity>;

template <std::size_t Capacity = 4096>
using SequenceGapRingT = ingest::SpscRing<SequenceGapEvent, Capacity>;

using DivergenceRing = DivergenceRingT<>;
using SequenceGapRing = SequenceGapRingT<>;

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
};

class Reconciler {
public:
    Reconciler(std::atomic<bool>& stop_flag,
               ingest::SpscRing<core::ExecEvent, 1u << 16>& primary,
               ingest::SpscRing<core::ExecEvent, 1u << 16>& dropcopy,
               OrderStateStore& store,
               ReconCounters& counters,
               DivergenceRing& divergence_ring,
               SequenceGapRing& seq_gap_ring,
               persist::AuditLogCounters* audit_counters = nullptr) noexcept;

    void run();
    void process_event_for_test(const ExecEvent& ev) noexcept { process_event(ev); }

private:
    void process_event(const ExecEvent& ev) noexcept;
    void increment_divergence_counter(DivergenceType type) noexcept;

    static constexpr std::int64_t qty_tolerance_ = 0;
    static constexpr std::int64_t px_tolerance_ = 0;
    static constexpr std::uint64_t timing_slack_ = 0;

    std::atomic<bool>& stop_flag_;
    ingest::SpscRing<core::ExecEvent, 1u << 16>& primary_;
    ingest::SpscRing<core::ExecEvent, 1u << 16>& dropcopy_;
    OrderStateStore& store_;
    ReconCounters& counters_;
    DivergenceRing& divergence_ring_;
    SequenceGapRing& seq_gap_ring_;
    persist::AuditLogCounters* audit_counters_;

    SequenceTracker primary_seq_tracker_{};
    SequenceTracker dropcopy_seq_tracker_{};
};

} // namespace core
