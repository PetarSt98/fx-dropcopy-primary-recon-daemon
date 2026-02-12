#pragma once

// Compile-time instrumentation macros for FX-7060 latency tracking.
//
// All macros compile to no-ops unless FX_PERF_ENABLED is defined.
// This ensures zero overhead in production builds when instrumentation
// is not needed, while allowing sub-microsecond measurement when enabled.
//
// Usage:
//   // At file scope (registers counter at first call):
//   PERF_DECLARE_COUNTER(recon_event, "recon.process_event")
//
//   // In hot path:
//   PERF_BEGIN(recon_event)
//   ... critical section ...
//   PERF_END(recon_event)
//
//   // Or use scoped RAII guard:
//   { PERF_SCOPED(recon_event) ... }
//
// Build:
//   cmake -DFX_PERF_ENABLED=ON ..   # enable instrumentation
//   cmake ..                          # disable (default) – zero overhead

#if defined(FX_PERF_ENABLED)

#include "util/perf_counters.hpp"
#include "util/rdtsc.hpp"
#include "util/tsc_calibration.hpp"

// ---------------------------------------------------------------------------
// Counter declaration (file scope).
// Registers the counter on first use via a helper function with an internal
// static. The file-scope variable `perf_cid_<tag>` holds the counter ID.
// ---------------------------------------------------------------------------
#define PERF_DECLARE_COUNTER(tag, name)                                       \
    static const ::util::PerfCounterRegistry::CounterId perf_cid_##tag =      \
        ::util::PerfCounterRegistry::instance().register_counter(name)

// ---------------------------------------------------------------------------
// Begin / End pair.  Store TSC at BEGIN; compute delta and record at END.
// The local variable `perf_tsc_##tag` is intentionally in the caller's scope
// so that BEGIN/END can be placed in the same function without braces.
// ---------------------------------------------------------------------------
#define PERF_BEGIN(tag)                                                        \
    const std::uint64_t perf_tsc_##tag = ::util::rdtsc()

#define PERF_END(tag)                                                         \
    do {                                                                      \
        const std::uint64_t perf_end_##tag = ::util::rdtsc();                 \
        const std::uint64_t perf_ns_##tag =                                   \
            ::util::tsc_to_ns(perf_end_##tag - perf_tsc_##tag);               \
        ::util::PerfCounterRegistry::instance().record(perf_cid_##tag,        \
                                                       perf_ns_##tag);        \
    } while (0)

// ---------------------------------------------------------------------------
// Scoped RAII guard – measures from construction to destruction.
// ---------------------------------------------------------------------------
namespace util { namespace detail {

class PerfScopedGuard {
public:
    explicit PerfScopedGuard(PerfCounterRegistry::CounterId id) noexcept
        : id_(id), start_(rdtsc()) {}

    ~PerfScopedGuard() noexcept {
        const std::uint64_t elapsed = rdtsc() - start_;
        PerfCounterRegistry::instance().record(id_, tsc_to_ns(elapsed));
    }

    PerfScopedGuard(const PerfScopedGuard&) = delete;
    PerfScopedGuard& operator=(const PerfScopedGuard&) = delete;

private:
    PerfCounterRegistry::CounterId id_;
    std::uint64_t start_;
};

}} // namespace util::detail

#define PERF_SCOPED(tag)                                                      \
    ::util::detail::PerfScopedGuard perf_guard_##tag(perf_cid_##tag)

// ---------------------------------------------------------------------------
// Dump all counters (call from shutdown / periodic reporter).
// ---------------------------------------------------------------------------
#define PERF_DUMP_ALL()                                                       \
    ::util::PerfCounterRegistry::instance().dump_all_to_stderr()

#define PERF_RESET_ALL()                                                      \
    ::util::PerfCounterRegistry::instance().reset_all()

#else // FX_PERF_ENABLED not defined – zero-overhead stubs

#define PERF_DECLARE_COUNTER(tag, name)  /*no-op*/
#define PERF_BEGIN(tag)                  /*no-op*/
#define PERF_END(tag)                    /*no-op*/
#define PERF_SCOPED(tag)                 /*no-op*/
#define PERF_DUMP_ALL()                  /*no-op*/
#define PERF_RESET_ALL()                 /*no-op*/

#endif // FX_PERF_ENABLED
