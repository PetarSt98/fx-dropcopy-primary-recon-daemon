#pragma once
// ---------------------------------------------------------------------------
// FX-8002: Async Journal Writer (hot loop → SPSC ring → background I/O)
//
// Repo discovery:
//   SPSC ring:  src/ingest/spsc_ring.hpp — SpscRing<T, N> with try_push/try_pop,
//               trivially-copyable element storage, power-of-two capacity.
//   FX-8001:    src/persist/record_format.hpp — encode_record_v1, decode_record_v1,
//               scan_records, kHeaderSize, kMaxPayloadBytes, RecordType.
//   CRC:        src/persist/crc32c.hpp — software CRC-32C.
//
// CMake targets modified:
//   fx_core          — add journal_writer.cpp
//   unit_tests_gtest — add journal_writer_tests.cpp, journal_writer_integration_tests.cpp
//
// Ring element max payload: 1024 bytes.
//   Rationale: ExecEvent and DivergenceEvent wire structs are well under 512 bytes.
//   1024 gives 2× headroom for future field growth while keeping ring elements
//   cache-friendly (each element ~1064 bytes total).
//
// v2.0 journal startup policy: truncate and start fresh.
//   Recovery will rely on snapshot + journal rotation in a later release.
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

#include "persist/record_format.hpp"

namespace persist {

// ---------------------------------------------------------------------------
// JournalStats — observable counters (all atomically published)
// ---------------------------------------------------------------------------
struct JournalStats {
    std::uint64_t enqueued     = 0;
    std::uint64_t written      = 0;
    std::uint64_t dropped      = 0;
    std::uint64_t io_errors    = 0;
    std::uint64_t bytes_written = 0;
    bool          degraded_io  = false; // set true on first I/O error or capacity exceeded
};

// ---------------------------------------------------------------------------
// JournalConfig
// ---------------------------------------------------------------------------
struct JournalConfig {
    std::filesystem::path path;                             // single file for v2.0
    std::uint64_t         segment_bytes      = 1ULL << 30;  // default 1 GiB
    std::size_t           ring_capacity_pow2  = 1u << 16;   // must be power-of-two
    std::uint64_t         consumer_sleep_ns   = 1'000;      // background thread idle sleep
};

// ---------------------------------------------------------------------------
// Ring element — fixed-size, trivially-copyable, no heap
// ---------------------------------------------------------------------------
inline constexpr std::uint32_t kMaxRingPayloadBytes = 1024;

struct RingElement {
    RecordType    type{};
    std::uint16_t flags{};
    std::uint64_t seqno{};
    std::uint32_t payload_len{};
    std::byte     payload[kMaxRingPayloadBytes]{};
};

static_assert(std::is_trivially_copyable_v<RingElement>,
              "RingElement must be trivially copyable for lock-free SPSC ring");

// ---------------------------------------------------------------------------
// JournalWriter
// ---------------------------------------------------------------------------
class JournalWriter {
public:
    explicit JournalWriter(JournalConfig cfg);
    ~JournalWriter();

    JournalWriter(const JournalWriter&)            = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;

    /// Opens file, preallocates to segment_bytes, launches background thread.
    bool start();

    /// Signals background thread to stop, joins, closes file.
    void stop();

    /// Non-blocking hot-path enqueue.  Returns false if dropped or not running.
    /// Guarantees: no syscalls, no locks, no heap allocations.
    bool enqueue_record(
        RecordType        type,
        std::uint16_t     flags,
        std::uint64_t     seqno,
        const std::byte*  payload,
        std::uint32_t     payload_len
    ) noexcept;

    /// Thread-safe snapshot of current stats.
    JournalStats stats() const noexcept;

private:
    // Background I/O thread entry point.
    void consumer_loop() noexcept;

    // Write a single ring element to the journal file.
    void write_element(const RingElement& elem) noexcept;

    // Preallocate journal file to cfg_.segment_bytes (cross-platform).
    bool preallocate_file();

    // Configuration
    JournalConfig cfg_;

    // SPSC ring — heap-allocated with runtime power-of-two capacity.
    // We use a flat array + atomic head/tail similar to ingest::SpscRing but
    // with runtime-configurable capacity (ring_capacity_pow2 from config).
    std::unique_ptr<RingElement[]> ring_buf_;
    std::size_t                    ring_mask_{0};
    alignas(64) std::atomic<std::size_t> ring_head_{0}; // producer writes
    alignas(64) std::atomic<std::size_t> ring_tail_{0}; // consumer writes

    // File I/O
    std::ofstream file_;
    std::uint64_t write_offset_{0};

    // Background thread
    std::atomic<bool> running_{false};
    std::thread       consumer_;

    // Scratch buffer for encoding (allocated once at start, reused)
    std::unique_ptr<std::byte[]> encode_buf_;
    std::size_t                  encode_buf_size_{0};

    // Stats (atomics for cross-thread reads)
    std::atomic<std::uint64_t> stat_enqueued_{0};
    std::atomic<std::uint64_t> stat_written_{0};
    std::atomic<std::uint64_t> stat_dropped_{0};
    std::atomic<std::uint64_t> stat_io_errors_{0};
    std::atomic<std::uint64_t> stat_bytes_written_{0};
    std::atomic<bool>          stat_degraded_io_{false};
};

} // namespace persist
