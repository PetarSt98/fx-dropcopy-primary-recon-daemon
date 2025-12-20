#pragma once

#include "core/exec_event.hpp"

namespace core {

inline constexpr bool is_terminal_status(OrdStatus s) noexcept {
    switch (s) {
    case OrdStatus::Filled:
    case OrdStatus::Canceled:
    case OrdStatus::Rejected:
        return true;
    default:
        return false;
    }
}

// Validate whether a transition between two OrdStatus values is acceptable for an
// exchange-grade order lifecycle. Unknown permits any first observation.
inline bool is_valid_transition(OrdStatus current, OrdStatus next) noexcept {
    using OS = OrdStatus;

    if (current == OS::Unknown) {
        return true; // First observation wins.
    }

    if (current == next) {
        // Idempotent repeats are acceptable (e.g., duplicate drop-copy messages).
        return true;
    }

    if (is_terminal_status(current)) {
        // Terminal states cannot transition back to active lifecycle states.
        return false;
    }

    switch (current) {
    case OS::New:
    case OS::PendingNew:
        return next == OS::Working || next == OS::PartiallyFilled || next == OS::Filled ||
               next == OS::CancelPending || next == OS::Rejected;
    case OS::Working:
        return next == OS::PartiallyFilled || next == OS::Filled || next == OS::CancelPending ||
               next == OS::Rejected;
    case OS::PartiallyFilled:
        return next == OS::PartiallyFilled || next == OS::Filled || next == OS::CancelPending;
    case OS::CancelPending:
        return next == OS::Canceled || next == OS::Rejected || next == OS::PartiallyFilled ||
               next == OS::Filled;
    case OS::Replaced:
        return next == OS::Working || next == OS::PartiallyFilled || next == OS::Filled ||
               next == OS::CancelPending || next == OS::Rejected;
    default:
        return false;
    }
}

// Apply a new status to the current lifecycle, returning whether the change was accepted.
inline bool apply_status_transition(OrdStatus& current, OrdStatus next) noexcept {
    if (!is_valid_transition(current, next)) {
        return false;
    }
    current = next;
    return true;
}

} // namespace core

