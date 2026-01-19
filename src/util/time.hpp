#pragma once

#include <chrono>
#include <cstdint>

namespace util {

// Get monotonic timestamp in nanoseconds.
// Used when no event timestamps are available for timer polling.
// Note: For deterministic replay, prefer event timestamps.
[[nodiscard]] inline std::uint64_t get_monotonic_ns() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

} // namespace util
