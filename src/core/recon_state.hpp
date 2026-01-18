#pragma once

#include <cstdint>
#include <type_traits>

namespace core {

// Reconciliation lifecycle state (NOT FIX OrdStatus)
enum class ReconState : std::uint8_t {
    Unknown = 0,        // Order not yet seen
    AwaitingPrimary,    // DropCopy seen, waiting for Primary
    AwaitingDropCopy,   // Primary seen, waiting for DropCopy
    InGrace,            // Both seen, mismatch detected, within grace period
    Matched,            // Both sides agree
    DivergedConfirmed,  // Mismatch persisted past grace
    SuppressedByGap     // Confirmation blocked due to open sequence gap
};

// Portable mismatch mask (exactly 1 byte, NOT using bitfields)
struct MismatchMask {
    std::uint8_t v{0};

    static constexpr std::uint8_t STATUS      = 1u << 0;
    static constexpr std::uint8_t CUM_QTY     = 1u << 1;
    static constexpr std::uint8_t LEAVES_QTY  = 1u << 2;
    static constexpr std::uint8_t AVG_PX      = 1u << 3;
    static constexpr std::uint8_t EXISTENCE   = 1u << 4;
    static constexpr std::uint8_t EXEC_ID     = 1u << 5;

    [[nodiscard]] constexpr bool any()  const noexcept { return v != 0; }
    [[nodiscard]] constexpr bool none() const noexcept { return v == 0; }
    [[nodiscard]] constexpr std::uint8_t bits() const noexcept { return v; }

    [[nodiscard]] constexpr bool has(std::uint8_t flag) const noexcept { return (v & flag) != 0; }
    constexpr void set(std::uint8_t flag) noexcept { v = static_cast<std::uint8_t>(v | flag); }
    constexpr void clear(std::uint8_t flag) noexcept { 
        v = static_cast<std::uint8_t>(v & static_cast<std::uint8_t>(~flag)); 
    }

    [[nodiscard]] constexpr bool operator==(const MismatchMask& o) const noexcept { return v == o.v; }
    [[nodiscard]] constexpr bool operator!=(const MismatchMask& o) const noexcept { return v != o.v; }
};

static_assert(sizeof(MismatchMask) == 1, "MismatchMask must be exactly 1 byte");
static_assert(alignof(MismatchMask) == 1, "MismatchMask must have 1-byte alignment");
static_assert(std::is_trivially_copyable_v<MismatchMask>, "MismatchMask must be trivially copyable");
static_assert(std::is_standard_layout_v<MismatchMask>, "MismatchMask must be standard layout");

// For debugging / tests only; do NOT wire into reconciler hot path logging.
[[nodiscard]] constexpr const char* to_string(ReconState s) noexcept {
    switch (s) {
        case ReconState::Unknown:            return "Unknown";
        case ReconState::AwaitingPrimary:    return "AwaitingPrimary";
        case ReconState::AwaitingDropCopy:   return "AwaitingDropCopy";
        case ReconState::InGrace:            return "InGrace";
        case ReconState::Matched:            return "Matched";
        case ReconState::DivergedConfirmed:  return "DivergedConfirmed";
        case ReconState::SuppressedByGap:    return "SuppressedByGap";
        default:                             return "Unknown";
    }
}

// Terminal means: no automatic transitions expected without new external info.
// For FX-7051, treat Matched and DivergedConfirmed as terminal.
[[nodiscard]] constexpr bool is_terminal_recon_state(ReconState s) noexcept {
    return s == ReconState::Matched || s == ReconState::DivergedConfirmed;
}

} // namespace core
