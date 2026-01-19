#pragma once

#include <cstdint>
#include <chrono>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace util {

inline uint64_t rdtsc(bool fence = false) noexcept {
#if defined(__i386__) || defined(__x86_64__)
    if (fence) {
#if defined(_MSC_VER)
        _mm_lfence();
#elif defined(__GNUC__)
        __asm__ __volatile__("lfence" ::: "memory");
#endif
    }
    unsigned int lo = 0, hi = 0;
#if defined(_MSC_VER)
    return __rdtsc();
#elif defined(__GNUC__)
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#else
    return 0;
#endif
#else
    (void)fence;
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

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
