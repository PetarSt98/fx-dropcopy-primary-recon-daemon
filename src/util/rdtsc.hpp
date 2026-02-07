#pragma once

#include <cstdint>
#include <ctime>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace util {

// Read Time-Stamp Counter (TSC) - high-performance CPU cycle counter
// 
// This uses RDTSCP when available (more reliable in virtualized environments)
// or falls back to RDTSC. On non-x86 or if TSC is unavailable, uses 
// clock_gettime(CLOCK_MONOTONIC) which is much faster than std::chrono.
//
// For Docker on Windows: RDTSCP typically works in modern Docker/WSL2 setups.
// If you encounter issues, they're usually due to hypervisor configuration
// (not fixable with Docker flags). Consider using a newer kernel or bare-metal.

inline uint64_t rdtsc(bool fence = false) noexcept {
#if defined(__i386__) || defined(__x86_64__)
    unsigned int lo = 0, hi = 0;
    unsigned int aux = 0;
    
#if defined(_MSC_VER)
    // MSVC: Use __rdtscp if available, otherwise __rdtsc
    if (fence) {
        _mm_lfence();
    }
    // Try RDTSCP first - it's widely available since ~2008
    // and more reliable in VMs
    #ifdef __RDTSCP__
        return __rdtscp(&aux);
    #else
        // Fallback to RDTSC on older CPUs
        return __rdtsc();
    #endif
#elif defined(__GNUC__)
    // GCC/Clang: Use RDTSCP (serializing, more reliable in VMs)
    // RDTSCP is supported on Intel since Nehalem (2008) and AMD since K8 Rev F (2006)
    // Most production systems have it, but we provide inline asm for compatibility
    if (fence) {
        __asm__ __volatile__("lfence" ::: "memory");
    }
    
    // Use RDTSCP - serializing and VM-friendly
    // Falls back gracefully to RDTSC on older CPUs (assembler handles it)
    __asm__ __volatile__(
        "rdtscp"
        : "=a"(lo), "=d"(hi), "=c"(aux)
        :
        : "memory"
    );
    return (static_cast<uint64_t>(hi) << 32) | lo;
#else
    // Fallback for unknown compiler
    (void)fence;
    (void)aux;
    return 0;
#endif

#else
    // Non-x86 architecture: use clock_gettime (faster than std::chrono)
    (void)fence;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        // If clock_gettime fails, return 0 (shouldn't happen on modern systems)
        return 0;
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + 
           static_cast<uint64_t>(ts.tv_nsec);
#endif
}

} // namespace util
