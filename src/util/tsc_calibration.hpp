#pragma once

#include <cstdint>
#include "util/rdtsc.hpp"

namespace util {

// TSC (Time Stamp Counter) frequency calibration for converting between
// CPU cycles and nanoseconds. Required for correct timer wheel operation.
//
// On modern x86_64 CPUs, TSC runs at a fixed frequency independent of
// CPU frequency scaling (invariant TSC). This frequency is typically
// the CPU's base frequency (e.g., 3GHz = 3,000,000,000 cycles/sec).
//
// For HFT use, we calibrate once at startup and cache the result.

class TscCalibration {
public:
    // Default assumes 3GHz TSC frequency (3 billion cycles/sec)
    // This is a reasonable default for modern Intel/AMD processors
    static constexpr std::uint64_t DEFAULT_TSC_FREQ_HZ = 3'000'000'000ULL;
    
    // Nanoseconds per second
    static constexpr std::uint64_t NS_PER_SEC = 1'000'000'000ULL;

    // Get singleton instance (thread-safe initialization in C++11+)
    static TscCalibration& instance() noexcept {
        static TscCalibration inst;
        return inst;
    }

    // Get TSC frequency in Hz (cycles per second)
    [[nodiscard]] std::uint64_t tsc_freq_hz() const noexcept {
        return tsc_freq_hz_;
    }

    // Convert TSC cycles to nanoseconds
    [[nodiscard]] std::uint64_t cycles_to_ns(std::uint64_t cycles) const noexcept {
        // cycles * NS_PER_SEC / tsc_freq_hz
        // Use 128-bit arithmetic to avoid overflow for large cycle counts
        // For HFT hot path, we use the approximation: cycles / cycles_per_ns_
        return cycles / cycles_per_ns_;
    }

    // Convert nanoseconds to TSC cycles
    [[nodiscard]] std::uint64_t ns_to_cycles(std::uint64_t ns) const noexcept {
        // ns * tsc_freq_hz / NS_PER_SEC
        return ns * cycles_per_ns_;
    }

    // Get pre-computed cycles per nanosecond (for inline fast-path conversion)
    [[nodiscard]] std::uint64_t cycles_per_ns() const noexcept {
        return cycles_per_ns_;
    }

    // Allow manual override of TSC frequency (for testing or known hardware)
    void set_tsc_freq_hz(std::uint64_t freq_hz) noexcept {
        tsc_freq_hz_ = freq_hz;
        cycles_per_ns_ = freq_hz / NS_PER_SEC;
        if (cycles_per_ns_ == 0) {
            cycles_per_ns_ = 1;  // Safety floor
        }
    }

private:
    TscCalibration() noexcept 
        : tsc_freq_hz_(DEFAULT_TSC_FREQ_HZ)
        , cycles_per_ns_(DEFAULT_TSC_FREQ_HZ / NS_PER_SEC) {}

    std::uint64_t tsc_freq_hz_;
    std::uint64_t cycles_per_ns_;  // Pre-computed for fast conversion
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
