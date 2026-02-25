#pragma once
// FX-8002: Async Journal Writer
//
// Design notes (repo discovery):
//   SPSC ring used  : custom inline ring with direct-slot access (NOT ingest::SpscRing
//                     which forces a full-element copy via try_push(const T&)).
//   CMake targets   : fx_core (library), unit_tests_gtest (tests).
//   Ring payload max: 512 bytes — WireExecEvent is 151 B, Divergence ~80 B;
//                     512 gives ample headroom without wasting cache per slot.
//
// Hot-path guarantees (enqueue_record):
//   • No syscalls, no locks, no heap allocations.
//   • Writes directly into ring slot — no stack temporary, no zero-init of payload.
//
// Background thread:
//   • Sequential file writes only (no seekp per record).
//   • Preallocated scratch buffer; no per-record heap churn.
//
// v2.0 policy: truncate file on start (fresh journal each run).
// Overflow policy: drop + counter (never blocks the producer).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

#include "persist/record_format.hpp"

namespace persist {

/// Max payload bytes that fit in one ring element.
static constexpr std::uint32_t kMaxRingPayload = 512;

struct JournalStats {
    std::uint64_t enqueued     = 0;
    std::uint64_t written      = 0;
    std::uint64_t dropped      = 0;
    std::uint64_t io_errors    = 0;
    std::uint64_t bytes_written = 0;
    bool          degraded_io  = false; // set on first I/O error or capacity exceeded
};

struct JournalConfig {
    std::filesystem::path path;                           // single journal file
    std::uint64_t         segment_bytes      = 1ULL << 30; // 1 GiB default
    std::size_t           ring_capacity_pow2 = 1u << 16;   // must be power-of-two
};

class JournalWriter {
public:
    explicit JournalWriter(JournalConfig cfg);
    ~JournalWriter();

    JournalWriter(const JournalWriter&)            = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;

    /// Open file, preallocate, launch background thread.  Idempotent.
    bool start();

    /// Signal thread, join, close file.  Idempotent.
    void stop();

    /// Non-blocking hot-path enqueue.  Returns false if dropped or not running.
    bool enqueue_record(RecordType     type,
                        std::uint16_t  flags,
                        std::uint64_t  seqno,
                        const std::byte* payload,
                        std::uint32_t  payload_len) noexcept;

    JournalStats stats() const noexcept;

private:
    // Fixed-size, trivially-copyable ring element.
    // payload[] is intentionally left uninitialized on the hot path.
    struct RingElement {
        RecordType    type;
        std::uint16_t flags;
        std::uint64_t seqno;
        std::uint32_t payload_len;
        std::byte     payload[kMaxRingPayload];
    };
    static_assert(std::is_trivially_copyable_v<RingElement>);

    void background_loop();

    JournalConfig config_;

    // Custom SPSC ring (direct slot access for zero-copy enqueue).
    std::unique_ptr<RingElement[]> ring_buf_;
    std::size_t                    ring_mask_{0};
    alignas(64) std::atomic<std::size_t> ring_head_{0};
    alignas(64) std::atomic<std::size_t> ring_tail_{0};

    // Atomic stats for cross-thread reads.
    std::atomic<std::uint64_t> stat_enqueued_{0};
    std::atomic<std::uint64_t> stat_written_{0};
    std::atomic<std::uint64_t> stat_dropped_{0};
    std::atomic<std::uint64_t> stat_io_errors_{0};
    std::atomic<std::uint64_t> stat_bytes_written_{0};
    std::atomic<bool>          stat_degraded_io_{false};

    // Thread control.
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_flag_{false};
    std::thread       bg_thread_;

    // File I/O (background thread only).
    std::ofstream  file_;
    std::uint64_t  write_offset_{0};

    // Scratch buffer for encode_record_v1 (background thread only).
    std::unique_ptr<std::byte[]> scratch_;
};

} // namespace persist
