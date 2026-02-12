#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "util/latency_histogram.hpp"

namespace util {

// PerfCounterRegistry provides a fixed-capacity, compile-time-sized registry
// of named latency histograms for system-wide performance instrumentation.
//
// Design:
// - All storage is pre-allocated (no heap allocation after construction)
// - Registration is O(N) scan but only called at startup
// - Lookup by ID is O(1) (direct index)
// - Thread safety: registration is NOT thread-safe (call during init only);
//   recording into histograms IS lock-free (single writer per histogram)
//
// Usage:
//   // At startup (before hot path):
//   auto& reg = PerfCounterRegistry::instance();
//   auto id = reg.register_counter("recon.process_event");
//
//   // Hot path:
//   reg.histogram(id).record(latency_ns);
//
//   // Reporting (periodic or on shutdown):
//   reg.dump_all_to_stderr();
class PerfCounterRegistry {
public:
    static constexpr std::size_t MAX_COUNTERS = 64;
    static constexpr std::size_t MAX_NAME_LEN = 48;

    using CounterId = std::uint32_t;
    static constexpr CounterId INVALID_ID = UINT32_MAX;

    static PerfCounterRegistry& instance() noexcept {
        static PerfCounterRegistry reg;
        return reg;
    }

    // Register a named counter. Returns INVALID_ID if registry is full or
    // name is empty. Idempotent: returns existing ID if name already registered.
    // NOT thread-safe – call only during initialisation.
    [[nodiscard]] CounterId register_counter(const char* name) noexcept {
        if (!name || name[0] == '\0') {
            return INVALID_ID;
        }

        // Check for existing registration (idempotent)
        for (std::size_t i = 0; i < size_; ++i) {
            if (std::strncmp(names_[i].data(), name, MAX_NAME_LEN) == 0) {
                return static_cast<CounterId>(i);
            }
        }

        if (size_ >= MAX_COUNTERS) {
            return INVALID_ID;
        }

        const auto id = static_cast<CounterId>(size_);
        std::strncpy(names_[id].data(), name, MAX_NAME_LEN - 1);
        names_[id][MAX_NAME_LEN - 1] = '\0';
        ++size_;
        return id;
    }

    // Access histogram by ID. Caller must ensure id < size().
    [[nodiscard]] LatencyHistogram& histogram(CounterId id) noexcept {
        return histograms_[id];
    }

    [[nodiscard]] const LatencyHistogram& histogram(CounterId id) const noexcept {
        return histograms_[id];
    }

    // Name of a registered counter.
    [[nodiscard]] const char* name(CounterId id) const noexcept {
        if (id >= size_) return "";
        return names_[id].data();
    }

    // Number of registered counters.
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    // Record a latency sample for counter `id`.
    void record(CounterId id, std::uint64_t ns) noexcept {
        if (id < size_) {
            histograms_[id].record(ns);
        }
    }

    // Reset all counters and histograms.
    // NOT thread-safe with concurrent record() calls.
    void reset_all() noexcept {
        for (std::size_t i = 0; i < size_; ++i) {
            histograms_[i].reset();
        }
    }

    // Dump all counters to stderr.
    void dump_all_to_stderr() const noexcept {
        std::fprintf(stderr, "\n=== Performance Counters ===\n");
        for (std::size_t i = 0; i < size_; ++i) {
            if (histograms_[i].count() > 0) {
                histograms_[i].dump_to_stderr(names_[i].data());
            }
        }
        std::fprintf(stderr, "============================\n\n");
    }

    // Dump all counters to a buffer.
    // Returns total characters written.
    std::size_t dump_all(char* buf, std::size_t buf_len) const noexcept {
        if (!buf || buf_len == 0) return 0;

        std::size_t offset = 0;
        for (std::size_t i = 0; i < size_; ++i) {
            if (histograms_[i].count() == 0) continue;
            if (offset >= buf_len - 1) break;

            int hdr = std::snprintf(buf + offset, buf_len - offset,
                                    "=== %s ===\n", names_[i].data());
            if (hdr > 0) offset += static_cast<std::size_t>(hdr);

            if (offset < buf_len - 1) {
                offset += histograms_[i].dump(buf + offset, buf_len - offset);
            }
            if (offset < buf_len - 1) {
                buf[offset++] = '\n';
            }
        }
        if (offset < buf_len) {
            buf[offset] = '\0';
        }
        return offset;
    }

private:
    PerfCounterRegistry() noexcept = default;

    std::array<std::array<char, MAX_NAME_LEN>, MAX_COUNTERS> names_{};
    std::array<LatencyHistogram, MAX_COUNTERS> histograms_{};
    std::size_t size_{0};
};

} // namespace util
