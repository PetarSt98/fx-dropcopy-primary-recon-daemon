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
// For Docker on Windows: TSC access typically works but if issues persist,
// run with: docker run --cap-add=SYS_TIME or configure Docker to expose TSC.

inline uint64_t rdtsc(bool fence = false) noexcept {
#if defined(__i386__) || defined(__x86_64__)
    unsigned int lo = 0, hi = 0;
    unsigned int aux = 0;
    
#if defined(_MSC_VER)
    // MSVC: Use __rdtscp if available, otherwise __rdtsc
    if (fence) {
        _mm_lfence();
    }
    #if defined(__AVX2__) || defined(__AVX__)
        // RDTSCP available on modern CPUs
        return __rdtscp(&aux);
    #else
        return __rdtsc();
    #endif
#elif defined(__GNUC__)
    // GCC/Clang: Try RDTSCP first (serializing, more reliable)
    // RDTSCP is supported on Intel since Nehalem (2008) and AMD since K8 Rev F (2006)
    if (fence) {
        __asm__ __volatile__("lfence" ::: "memory");
    }
    
    // Try RDTSCP (more VM-friendly, serializing)
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
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + 
           static_cast<uint64_t>(ts.tv_nsec);
#endif
}

} // namespace util
