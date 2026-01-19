#pragma once

#include <cstdint>

#include "core/exec_event.hpp"

namespace core {

enum class GapKind : std::uint8_t {
    Gap,
    Duplicate,
    OutOfOrder
};

struct SequenceGapEvent {
    Source source{};
    std::uint16_t session_id{0};
    std::uint64_t expected_seq{0};
    std::uint64_t seen_seq{0};
    GapKind kind{GapKind::Gap};
    std::uint64_t detect_ts{0};
};

struct SequenceTracker {
    std::uint64_t expected_seq{0};
    std::uint64_t last_seen_seq{0};
    bool initialized{false};
    bool gap_open{false};
    std::uint64_t gap_start_seq{0};
    std::uint16_t gap_epoch{0};  // Incremented each time a new gap is detected (FX-7053)
    
    // ===== FX-7054: Enhanced gap lifecycle tracking =====
    std::uint64_t gap_opened_tsc{0};          // When gap was first detected (TSC cycles)
    std::uint64_t gap_first_missing_seq{0};   // First missing sequence (inclusive)
    std::uint64_t gap_last_missing_seq{0};    // Last missing sequence (inclusive)
    std::uint32_t orders_in_gap_uncertainty{0};  // Count of orders affected by this gap
};

inline bool init_sequence_tracker(SequenceTracker& trk, std::uint64_t first_seq) noexcept {
    if (trk.initialized) {
        return false;
    }
    trk.initialized = true;
    trk.last_seen_seq = first_seq;
    trk.expected_seq = first_seq + 1;
    trk.gap_open = false;
    trk.gap_start_seq = 0;
    return true;
}

inline bool track_sequence(SequenceTracker& trk,
                           Source source,
                           std::uint16_t session_id,
                           std::uint64_t seq,
                           std::uint64_t now_ts,
                           SequenceGapEvent* out_event) noexcept {
    if (!trk.initialized) {
        init_sequence_tracker(trk, seq);
        return false;
    }

    if (seq == trk.expected_seq) {
        trk.last_seen_seq = seq;
        trk.expected_seq = seq + 1;
        
        // FX-7054: Check if gap should close
        // Gap closes when expected_seq advances past gap_last_missing_seq
        if (trk.gap_open && trk.expected_seq > trk.gap_last_missing_seq) {
            trk.gap_open = false;
            trk.gap_start_seq = 0;
            trk.gap_opened_tsc = 0;
            trk.gap_first_missing_seq = 0;
            trk.gap_last_missing_seq = 0;
            // Note: gap_epoch and orders_in_gap_uncertainty are preserved (historical)
        }
        
        return false;
    }

    if (seq > trk.expected_seq) {
        const std::uint64_t expected_before = trk.expected_seq;
        
        // Open or extend gap
        if (!trk.gap_open) {
            // NEW gap detected
            trk.gap_open = true;
            trk.gap_start_seq = trk.expected_seq;
            trk.gap_opened_tsc = now_ts;  // FX-7054
            trk.gap_first_missing_seq = trk.expected_seq;  // FX-7054
            ++trk.gap_epoch;
        }
        
        // Update last missing (gap may be extending)
        trk.gap_last_missing_seq = seq - 1;  // FX-7054
        
        trk.last_seen_seq = seq;
        trk.expected_seq = seq + 1;

        if (out_event) {
            out_event->source = source;
            out_event->session_id = session_id;
            out_event->expected_seq = expected_before;
            out_event->seen_seq = seq;
            out_event->kind = GapKind::Gap;
            out_event->detect_ts = now_ts;
        }
        return true;
    }

    // seq < expected_seq
    if (out_event) {
        out_event->source = source;
        out_event->session_id = session_id;
        out_event->expected_seq = trk.expected_seq;
        out_event->seen_seq = seq;
        out_event->kind = (seq == trk.last_seen_seq) ? GapKind::Duplicate : GapKind::OutOfOrder;
        out_event->detect_ts = now_ts;
    }
    return true;
}

} // namespace core
