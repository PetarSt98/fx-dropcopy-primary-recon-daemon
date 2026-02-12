#pragma once

#include "util/perf_counters.hpp"
#include "util/rdtsc.hpp"
#include "util/tsc_calibration.hpp"

namespace util {

#ifdef FX_PERF_ENABLED

// RAII guard for scoped latency measurement
class PerfScopeGuard {
public:
    explicit PerfScopeGuard(PerfCounterId counter_id) noexcept
        : counter_id_(counter_id), start_tsc_(rdtsc()) {}
    
    ~PerfScopeGuard() noexcept {
        const std::uint64_t end_tsc = rdtsc();
        const std::uint64_t elapsed_tsc = end_tsc - start_tsc_;
        const std::uint64_t elapsed_ns = tsc_to_ns(elapsed_tsc);
        PerfRegistry::instance().get(counter_id_).record_latency(elapsed_ns);
    }
    
    PerfScopeGuard(const PerfScopeGuard&) = delete;
    PerfScopeGuard& operator=(const PerfScopeGuard&) = delete;
    
private:
    PerfCounterId counter_id_;
    std::uint64_t start_tsc_;
};

// Helper macro for unique variable name generation
#define PERF_CONCAT_IMPL(a, b) a##b
#define PERF_CONCAT(a, b) PERF_CONCAT_IMPL(a, b)

// Macro: Scoped latency measurement
// Usage: PERF_SCOPE(PerfCounterId::ReconcilerProcessEvent);
// Uses __COUNTER__ to ensure unique variable names even on the same line
#define PERF_SCOPE(counter_id) \
    [[maybe_unused]] ::util::PerfScopeGuard PERF_CONCAT(_perf_guard_, __COUNTER__)(counter_id)

// Macro: Manual start timing
// Usage: PERF_START(my_timer);
#define PERF_START(var_name) \
    const std::uint64_t var_name##_start_tsc = ::util::rdtsc()

// Macro: Manual stop timing and record
// Usage: PERF_STOP(my_timer, PerfCounterId::HashTableLookup);
#define PERF_STOP(var_name, counter_id) \
    do { \
        const std::uint64_t var_name##_end_tsc = ::util::rdtsc(); \
        const std::uint64_t var_name##_elapsed_tsc = var_name##_end_tsc - var_name##_start_tsc; \
        const std::uint64_t var_name##_elapsed_ns = ::util::tsc_to_ns(var_name##_elapsed_tsc); \
        ::util::PerfRegistry::instance().get(counter_id).record_latency(var_name##_elapsed_ns); \
    } while (0)

// Macro: Simple counter increment (no latency)
// Usage: PERF_COUNT(PerfCounterId::AeronPoll);
#define PERF_COUNT(counter_id) \
    ::util::PerfRegistry::instance().get(counter_id).increment()

// Macro: Conditional execution when perf is enabled
// Usage: PERF_IF_ENABLED({ expensive_debug_code(); });
#define PERF_IF_ENABLED(body) \
    do { body } while (0)

#else

// When FX_PERF_ENABLED is not defined, all macros compile to no-ops

#define PERF_SCOPE(counter_id) ((void)0)
#define PERF_START(var_name) ((void)0)
#define PERF_STOP(var_name, counter_id) ((void)0)
#define PERF_COUNT(counter_id) ((void)0)
#define PERF_IF_ENABLED(body) ((void)0)

#endif // FX_PERF_ENABLED

} // namespace util
