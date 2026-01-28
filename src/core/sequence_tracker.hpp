#pragma once

#include <cstdint>

#include "core/exec_event.hpp"

namespace core {

enum class GapKind : std::uint8_t {
    Gap,
    Duplicate,
    OutOfOrder,
    GapFill  // Out-of-order message that fills/closes a gap
};

struct SequenceGapEvent {
    Source source{};
    std::uint16_t session_id{0};
    std::uint64_t expected_seq{0};
    std::uint64_t seen_seq{0};
    GapKind kind{GapKind::Gap};
    std::uint64_t detect_ts{0};
    bool gap_closed_by_fill{false};  // True if this event closed a gap by fill
};

struct SequenceTracker {
    std::uint64_t expected_seq{0};
    std::uint64_t last_seen_seq{0};
    bool initialized{false};
    bool gap_open{false};
    std::uint64_t gap_start_seq{0};
    std::uint64_t gap_end_seq{0};       // End of gap range (exclusive) - seq we jumped to
    std::uint64_t gap_detected_tsc{0};  // TSC when gap was detected (for timeout)
    std::uint32_t gap_epoch{0};         // Incremented each time a new gap is detected (FX-7053)
                                        // Using uint32_t to avoid wrap-around issues (would require 4B+ gaps)
                                        // IMPORTANT: Value 0 is reserved as sentinel (means "not flagged").
                                        // Epochs start at 1 and skip 0 on wrap-around.
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
    trk.gap_end_seq = 0;
    trk.gap_detected_tsc = 0;
    return true;
}

// Close the gap explicitly (e.g., after timeout or gap fill detection)
inline void close_gap(SequenceTracker& trk) noexcept {
    trk.gap_open = false;
    trk.gap_start_seq = 0;
    trk.gap_end_seq = 0;
    trk.gap_detected_tsc = 0;
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
        return false;
    }

    if (seq > trk.expected_seq) {
        const std::uint64_t expected_before = trk.expected_seq;
        trk.gap_open = true;
        trk.gap_start_seq = trk.expected_seq;
        trk.gap_end_seq = seq;  // The sequence we jumped to (exclusive end of gap)
        trk.gap_detected_tsc = now_ts;  // Record when gap was detected for timeout
        trk.last_seen_seq = seq;
        trk.expected_seq = seq + 1;
        // Increment epoch each time a new gap is detected (FX-7053)
        // Skip 0 on wrap-around since 0 is reserved as sentinel ("not flagged")
        ++trk.gap_epoch;
        if (trk.gap_epoch == 0) {
            trk.gap_epoch = 1;  // Skip sentinel value
        }

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

    // seq < expected_seq (out-of-order or duplicate)
    const bool is_duplicate = (seq == trk.last_seen_seq);
    
    // Check if this out-of-order message fills part of the gap
    // If so, we may be able to close the gap
    bool gap_closed_by_fill = false;
    if (trk.gap_open && !is_duplicate) {
        // Check if seq falls within the gap range [gap_start_seq, gap_end_seq)
        if (seq >= trk.gap_start_seq && seq < trk.gap_end_seq) {
            // This message is filling the gap. For simplicity, close the gap
            // when we receive ANY message in the gap range. A more sophisticated
            // approach would track exactly which sequences are still missing.
            // NOTE: In production, you might want to require ALL missing sequences
            // to arrive before closing, but for HFT the timeout approach is safer.
            close_gap(trk);
            gap_closed_by_fill = true;
        }
    }

    if (out_event) {
        out_event->source = source;
        out_event->session_id = session_id;
        out_event->expected_seq = trk.expected_seq;
        out_event->seen_seq = seq;
        if (is_duplicate) {
            out_event->kind = GapKind::Duplicate;
        } else if (gap_closed_by_fill) {
            out_event->kind = GapKind::GapFill;
        } else {
            out_event->kind = GapKind::OutOfOrder;
        }
        out_event->detect_ts = now_ts;
        out_event->gap_closed_by_fill = gap_closed_by_fill;
    }
    return true;
}

} // namespace core
