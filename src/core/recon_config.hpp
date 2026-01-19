#pragma once

#include <cstdint>
#include <type_traits>

namespace core {

// Configuration for two-stage reconciliation behavior.
// All time values are in nanoseconds.
struct ReconConfig {
    // Grace period: time to wait before confirming a divergence
    // Orders that resolve within this window don't generate confirmed divergences
    std::uint64_t grace_period_ns{500'000'000};  // 500ms default

    // Gap recheck period: when suppressed by gap, how long to wait before rechecking
    std::uint64_t gap_recheck_period_ns{100'000'000};  // 100ms default

    // Deduplication window: don't re-emit identical divergence within this period
    std::uint64_t divergence_dedup_window_ns{1'000'000'000};  // 1s default

    // Tolerances for mismatch detection
    std::int64_t qty_tolerance{0};    // Quantity tolerance (0 = exact match)
    std::int64_t px_tolerance{0};     // Price tolerance in micro-units (0 = exact match)
    std::uint64_t timing_slack_ns{0}; // Timing tolerance (0 = exact match)

    // Feature flags
    bool enable_windowed_recon{true};   // Enable two-stage pipeline (can disable for A/B testing)
    bool enable_gap_suppression{true};  // Suppress divergences during sequence gaps
};

static_assert(std::is_trivially_copyable_v<ReconConfig>, "ReconConfig must be trivially copyable");

// Default configuration suitable for production
[[nodiscard]] inline constexpr ReconConfig default_recon_config() noexcept {
    return ReconConfig{};
}

} // namespace core
