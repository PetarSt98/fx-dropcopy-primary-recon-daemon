#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include "util/rdtsc.hpp"

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #ifdef _MSC_VER
        #include <intrin.h>
        #define CPUID(regs, leaf) __cpuid(reinterpret_cast<int*>(regs), leaf)
        #define CPUID_EX(regs, leaf, subleaf) __cpuidex(reinterpret_cast<int*>(regs), leaf, subleaf)
    #else
        #include <cpuid.h>
        #define CPUID(regs, leaf) __cpuid(leaf, (regs)[0], (regs)[1], (regs)[2], (regs)[3])
        #define CPUID_EX(regs, leaf, subleaf) __cpuid_count(leaf, subleaf, (regs)[0], (regs)[1], (regs)[2], (regs)[3])
    #endif
#endif

namespace util {

// TSC (Time Stamp Counter) frequency calibration for converting between
// CPU cycles and nanoseconds. Required for correct timer wheel operation.
//
// Features:
// - Runtime calibration using clock_gettime(CLOCK_MONOTONIC)
// - Q32.32 fixed-point arithmetic for sub-nanosecond precision
// - Invariant TSC validation via CPUID
//
// On modern x86_64 CPUs, TSC runs at a fixed frequency independent of
// CPU frequency scaling (invariant TSC). This frequency is typically
// the CPU's base frequency (e.g., 3GHz = 3,000,000,000 cycles/sec).

class TscCalibration {
public:
    // Default assumes 3GHz TSC frequency (3 billion cycles/sec)
    // Used as fallback if runtime calibration fails
    static constexpr std::uint64_t DEFAULT_TSC_FREQ_HZ = 3'000'000'000ULL;
    
    // Nanoseconds per second
    static constexpr std::uint64_t NS_PER_SEC = 1'000'000'000ULL;
    
    // Q32.32 fixed-point shift (32 fractional bits)
    static constexpr unsigned FRAC_BITS = 32;
    static constexpr std::uint64_t FRAC_SCALE = 1ULL << FRAC_BITS;

    // Get singleton instance (thread-safe initialization in C++11+)
    // NOTE: Does NOT auto-calibrate. Call calibrate_blocking() during startup.
    static TscCalibration& instance() noexcept {
        static TscCalibration inst;
        return inst;
    }

    // Check if CPU supports invariant TSC (constant rate regardless of power state)
    [[nodiscard]] static bool has_invariant_tsc() noexcept {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
        std::uint32_t regs[4] = {0, 0, 0, 0};
        
        // Check for extended CPUID support
        CPUID(regs, 0x80000000U);
        if (regs[0] < 0x80000007U) {
            return false;  // Extended CPUID not supported
        }
        
        // CPUID.80000007H:EDX[8] = invariant TSC
        CPUID(regs, 0x80000007U);
        return (regs[3] & (1U << 8)) != 0;
#else
        return false;  // Non-x86 architecture
#endif
    }

    // Runtime calibration using wall-clock time measurement
    // Returns true if calibration succeeded, false if it failed (uses default)
    bool calibrate(std::uint64_t duration_ms = 100) noexcept {
        struct timespec start_ts{}, end_ts{};
        
        // Get starting timestamps
        if (clock_gettime(CLOCK_MONOTONIC, &start_ts) != 0) {
            return false;
        }
        const std::uint64_t tsc_start = rdtsc();
        
        // Sleep for measurement duration
        struct timespec sleep_ts{};
        sleep_ts.tv_sec = static_cast<time_t>(duration_ms / 1000);
        sleep_ts.tv_nsec = static_cast<long>((duration_ms % 1000) * 1'000'000);
        nanosleep(&sleep_ts, nullptr);
        
        // Get ending timestamps
        const std::uint64_t tsc_end = rdtsc();
        if (clock_gettime(CLOCK_MONOTONIC, &end_ts) != 0) {
            return false;
        }
        
        // Calculate elapsed wall-clock time in nanoseconds
        const std::uint64_t elapsed_ns = 
            static_cast<std::uint64_t>(end_ts.tv_sec - start_ts.tv_sec) * NS_PER_SEC +
            static_cast<std::uint64_t>(end_ts.tv_nsec - start_ts.tv_nsec);
        
        if (elapsed_ns == 0) {
            return false;
        }
        
        // Calculate TSC frequency: cycles_delta * 1e9 / elapsed_ns
        const std::uint64_t tsc_delta = tsc_end - tsc_start;
        const std::uint64_t measured_freq = (tsc_delta * NS_PER_SEC) / elapsed_ns;
        
        if (measured_freq < 100'000'000ULL || measured_freq > 10'000'000'000ULL) {
            // Sanity check: frequency should be between 100MHz and 10GHz
            return false;
        }
        
        set_tsc_freq_hz(measured_freq);
        calibrated_ = true;
        
        // Log measured frequency for monitoring (HFT systems typically log to stderr)
        std::fprintf(stderr, "[TscCalibration] Measured TSC frequency: %lu Hz (%.3f GHz)\n",
                     static_cast<unsigned long>(measured_freq),
                     static_cast<double>(measured_freq) / 1e9);
        
        return true;
    }

    // Get TSC frequency in Hz (cycles per second)
    [[nodiscard]] std::uint64_t tsc_freq_hz() const noexcept {
        return tsc_freq_hz_;
    }
    
    // Check if runtime calibration was performed
    [[nodiscard]] bool is_calibrated() const noexcept {
        return calibrated_;
    }

    // Convert TSC cycles to nanoseconds using Q32.32 fixed-point
    // cycles_to_ns = cycles * ns_per_cycle_q32 >> 32
    [[nodiscard]] std::uint64_t cycles_to_ns(std::uint64_t cycles) const noexcept {
        // Use 128-bit multiplication to avoid overflow
        // Result: (cycles * ns_per_cycle_q32) >> 32
#if defined(__SIZEOF_INT128__)
        __uint128_t product = static_cast<__uint128_t>(cycles) * ns_per_cycle_q32_;
        return static_cast<std::uint64_t>(product >> FRAC_BITS);
#else
        // Fallback for platforms without 128-bit integers
        // Split into high/low parts to avoid overflow
        const std::uint64_t cycles_hi = cycles >> 32;
        const std::uint64_t cycles_lo = cycles & 0xFFFFFFFFULL;
        const std::uint64_t ns_hi = ns_per_cycle_q32_ >> 32;
        const std::uint64_t ns_lo = ns_per_cycle_q32_ & 0xFFFFFFFFULL;
        
        // Full product = (cycles_hi * 2^32 + cycles_lo) * (ns_hi * 2^32 + ns_lo)
        // We need bits [95:32] of the 128-bit result
        const std::uint64_t lo_lo = cycles_lo * ns_lo;
        const std::uint64_t lo_hi = cycles_lo * ns_hi;
        const std::uint64_t hi_lo = cycles_hi * ns_lo;
        const std::uint64_t hi_hi = cycles_hi * ns_hi;
        
        const std::uint64_t mid = (lo_lo >> 32) + (lo_hi & 0xFFFFFFFFULL) + (hi_lo & 0xFFFFFFFFULL);
        return hi_hi + (lo_hi >> 32) + (hi_lo >> 32) + (mid >> 32);
#endif
    }

    // Convert nanoseconds to TSC cycles using Q32.32 fixed-point
    // ns_to_cycles = ns * cycles_per_ns_q32 >> 32
    [[nodiscard]] std::uint64_t ns_to_cycles(std::uint64_t ns) const noexcept {
#if defined(__SIZEOF_INT128__)
        __uint128_t product = static_cast<__uint128_t>(ns) * cycles_per_ns_q32_;
        return static_cast<std::uint64_t>(product >> FRAC_BITS);
#else
        // Fallback using same technique as cycles_to_ns
        const std::uint64_t ns_hi = ns >> 32;
        const std::uint64_t ns_lo = ns & 0xFFFFFFFFULL;
        const std::uint64_t cpn_hi = cycles_per_ns_q32_ >> 32;
        const std::uint64_t cpn_lo = cycles_per_ns_q32_ & 0xFFFFFFFFULL;
        
        const std::uint64_t lo_lo = ns_lo * cpn_lo;
        const std::uint64_t lo_hi = ns_lo * cpn_hi;
        const std::uint64_t hi_lo = ns_hi * cpn_lo;
        const std::uint64_t hi_hi = ns_hi * cpn_hi;
        
        const std::uint64_t mid = (lo_lo >> 32) + (lo_hi & 0xFFFFFFFFULL) + (hi_lo & 0xFFFFFFFFULL);
        return hi_hi + (lo_hi >> 32) + (hi_lo >> 32) + (mid >> 32);
#endif
    }

    // Legacy accessor for backward compatibility
    [[nodiscard]] std::uint64_t cycles_per_ns() const noexcept {
        return cycles_per_ns_q32_ >> FRAC_BITS;  // Return integer part only
    }

    // Allow manual override of TSC frequency (for testing or known hardware)
    void set_tsc_freq_hz(std::uint64_t freq_hz) noexcept {
        tsc_freq_hz_ = freq_hz;
        
        // Compute Q32.32 fixed-point conversion factors
        // cycles_per_ns_q32 = (freq_hz << 32) / NS_PER_SEC
        // ns_per_cycle_q32 = (NS_PER_SEC << 32) / freq_hz
        
        if (freq_hz == 0) {
            freq_hz = DEFAULT_TSC_FREQ_HZ;  // Prevent division by zero
        }
        
#if defined(__SIZEOF_INT128__)
        cycles_per_ns_q32_ = static_cast<std::uint64_t>(
            (static_cast<__uint128_t>(freq_hz) << FRAC_BITS) / NS_PER_SEC
        );
        ns_per_cycle_q32_ = static_cast<std::uint64_t>(
            (static_cast<__uint128_t>(NS_PER_SEC) << FRAC_BITS) / freq_hz
        );
#else
        // Fallback: use double for intermediate calculation, then convert
        const double cycles_per_ns = static_cast<double>(freq_hz) / static_cast<double>(NS_PER_SEC);
        const double ns_per_cycle = static_cast<double>(NS_PER_SEC) / static_cast<double>(freq_hz);
        cycles_per_ns_q32_ = static_cast<std::uint64_t>(cycles_per_ns * static_cast<double>(FRAC_SCALE));
        ns_per_cycle_q32_ = static_cast<std::uint64_t>(ns_per_cycle * static_cast<double>(FRAC_SCALE));
#endif
        
        // Safety floor for cycles_per_ns (at least 1 in integer part)
        if (cycles_per_ns_q32_ < FRAC_SCALE) {
            cycles_per_ns_q32_ = FRAC_SCALE;
        }
    }

    // Explicit calibration method for startup (NOT in hot path!)
    // Call this during application initialization before any timing-critical code
    // Blocks for ~duration_ms to measure TSC frequency
    void calibrate_blocking(std::uint64_t duration_ms = 100) noexcept {
        if (has_invariant_tsc()) {
            calibrate(duration_ms);
        } else {
            std::fprintf(stderr, "[TscCalibration] WARNING: Invariant TSC not detected. "
                                 "Using default frequency: %lu Hz\n",
                         static_cast<unsigned long>(DEFAULT_TSC_FREQ_HZ));
        }
    }

private:
    TscCalibration() noexcept 
        : tsc_freq_hz_(DEFAULT_TSC_FREQ_HZ)
        , cycles_per_ns_q32_(0)
        , ns_per_cycle_q32_(0)
        , calibrated_(false)
    {
        // Initialize with default frequency only - NO BLOCKING!
        // Calibration must be called explicitly via calibrate_blocking()
        // during application startup (not in hot path)
        set_tsc_freq_hz(DEFAULT_TSC_FREQ_HZ);
    }

    std::uint64_t tsc_freq_hz_;           // TSC frequency in Hz
    std::uint64_t cycles_per_ns_q32_;     // Q32.32 fixed-point: cycles per nanosecond
    std::uint64_t ns_per_cycle_q32_;      // Q32.32 fixed-point: nanoseconds per cycle
    bool calibrated_;                      // True if runtime calibration succeeded
};

// Convenience free functions for common operations

// Convert nanoseconds to TSC cycles
[[nodiscard]] inline std::uint64_t ns_to_tsc(std::uint64_t ns) noexcept {
    return TscCalibration::instance().ns_to_cycles(ns);
}

// Convert TSC cycles to nanoseconds  
[[nodiscard]] inline std::uint64_t tsc_to_ns(std::uint64_t cycles) noexcept {
    return TscCalibration::instance().cycles_to_ns(cycles);
}

// Get current time in nanoseconds (via TSC conversion)
[[nodiscard]] inline std::uint64_t now_ns() noexcept {
    return tsc_to_ns(rdtsc());
}

} // namespace util
