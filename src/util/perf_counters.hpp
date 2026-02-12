#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#ifdef FX_PERF_ENABLED
#include "util/latency_histogram.hpp"
#endif

namespace util {

// Performance counter identifiers for all instrumented operations
enum class PerfCounterId : std::size_t {
    ReconcilerProcessEvent = 0,
    HashTableLookup,
    HashTableUpsert,
    ArenaAllocate,
    TimerWheelSchedule,
    TimerWheelPollExpired,
    SpscRingPush,
    SpscRingPop,
    AeronPoll,
    MismatchCompute,
    COUNT  // Must be last - represents total count of counter IDs
};

#ifdef FX_PERF_ENABLED

// PerfCounter - Tracks operation count and latency histogram
struct PerfCounter {
    std::uint64_t count{0};
    LatencyHistogram<2048, 20'000> latency_hist;  // 2048 buckets, max 20us (~9.8ns bins)
    
    void record_latency(std::uint64_t ns) noexcept {
        ++count;
        latency_hist.record(ns);
    }
    
    void increment() noexcept {
        ++count;
    }
    
    void reset() noexcept {
        count = 0;
        latency_hist.reset();
    }
};

// PerfRegistry - Global singleton for performance counters
class PerfRegistry {
public:
    static PerfRegistry& instance() noexcept {
        static PerfRegistry inst;
        return inst;
    }
    
    // Get counter by ID
    [[nodiscard]] PerfCounter& get(PerfCounterId id) noexcept {
        return counters_[static_cast<std::size_t>(id)];
    }
    
    // Reset all counters
    void reset() noexcept {
        for (auto& counter : counters_) {
            counter.reset();
        }
    }
    
    // Dump all counters to stderr
    void dump() const noexcept {
        std::fprintf(stderr, "\n=== Performance Counters ===\n");
        
        dump_counter(PerfCounterId::ReconcilerProcessEvent, "ReconcilerProcessEvent");
        dump_counter(PerfCounterId::HashTableLookup, "HashTableLookup");
        dump_counter(PerfCounterId::HashTableUpsert, "HashTableUpsert");
        dump_counter(PerfCounterId::ArenaAllocate, "ArenaAllocate");
        dump_counter(PerfCounterId::TimerWheelSchedule, "TimerWheelSchedule");
        dump_counter(PerfCounterId::TimerWheelPollExpired, "TimerWheelPollExpired");
        dump_counter(PerfCounterId::SpscRingPush, "SpscRingPush");
        dump_counter(PerfCounterId::SpscRingPop, "SpscRingPop");
        dump_counter(PerfCounterId::AeronPoll, "AeronPoll");
        dump_counter(PerfCounterId::MismatchCompute, "MismatchCompute");
        
        std::fprintf(stderr, "\n");
    }
    
private:
    PerfRegistry() noexcept = default;
    
    void dump_counter(PerfCounterId id, const char* name) const noexcept {
        const auto& counter = counters_[static_cast<std::size_t>(id)];
        if (counter.count > 0) {
            std::fprintf(stderr, "\n%s: %lu calls\n", name, 
                        static_cast<unsigned long>(counter.count));
            if (counter.latency_hist.count() > 0) {
                std::fprintf(stderr, "  Min: %lu ns | Mean: %lu ns | Max: %lu ns\n",
                           static_cast<unsigned long>(counter.latency_hist.min()),
                           static_cast<unsigned long>(counter.latency_hist.mean()),
                           static_cast<unsigned long>(counter.latency_hist.max()));
                std::fprintf(stderr, "  P50: %lu ns | P99: %lu ns | P99.9: %lu ns\n",
                           static_cast<unsigned long>(counter.latency_hist.percentile(0.50)),
                           static_cast<unsigned long>(counter.latency_hist.percentile(0.99)),
                           static_cast<unsigned long>(counter.latency_hist.percentile(0.999)));
            }
        }
    }
    
    std::array<PerfCounter, static_cast<std::size_t>(PerfCounterId::COUNT)> counters_{};
};

#else

// When FX_PERF_ENABLED is not defined, PerfRegistry is a minimal no-op struct
struct PerfRegistry {
    static PerfRegistry& instance() noexcept {
        static PerfRegistry inst;
        return inst;
    }
    
    void reset() noexcept {}
    void dump() const noexcept {}
};

#endif // FX_PERF_ENABLED

} // namespace util
