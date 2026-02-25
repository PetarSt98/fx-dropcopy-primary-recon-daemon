#pragma once

// ---------------------------------------------------------------------------
// FX-8002: Async Journal Writer
// ---------------------------------------------------------------------------
// FX-8001 reuse: encode_record_v1, decode_record_v1, scan_records from
//   src/persist/record_format.hpp  (also crc32c.hpp for checksum)
// ingest::SpscRing: NOT modified — this is a purpose-built SPSC ring internal
//   to JournalWriter with runtime capacity and direct-slot writes.
// MAX_PAYLOAD = 512 bytes — sufficient for ExecEvent/DivergenceEvent payloads
//   (wire_exec_event.hpp fields total ~200 bytes serialized; 512 provides
//    headroom for future fields without waste).
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr std::uint32_t kJournalMaxPayload = 512;

// ---------------------------------------------------------------------------
// JournalStats — returned as an atomic snapshot
// ---------------------------------------------------------------------------
struct JournalStats {
    std::uint64_t enqueued      = 0;
    std::uint64_t written       = 0;
    std::uint64_t dropped       = 0;
    std::uint64_t io_errors     = 0;
    std::uint64_t bytes_written = 0;
    bool          degraded_io   = false;
};

// ---------------------------------------------------------------------------
// JournalConfig
// ---------------------------------------------------------------------------
struct JournalConfig {
    std::filesystem::path path;
    std::uint64_t  segment_bytes       = 1ULL << 30;   // 1 GiB
    std::size_t    ring_capacity_pow2  = 1u << 16;      // 65 536
    std::uint32_t  max_payload_bytes   = 512;            // fixed at start()
    std::uint64_t  idle_sleep_ns       = 1000;
};

// ---------------------------------------------------------------------------
// RingElement — fixed-size envelope for the SPSC queue
// ---------------------------------------------------------------------------
struct RingElement {
    RecordType    type;
    std::uint16_t flags;
    std::uint64_t seqno;
    std::uint32_t payload_len;
    std::byte     payload[kJournalMaxPayload];
};
static_assert(std::is_trivially_copyable_v<RingElement>,
              "RingElement must be trivially copyable");

// ---------------------------------------------------------------------------
// JournalWriter
// ---------------------------------------------------------------------------
class JournalWriter {
public:
    explicit JournalWriter(JournalConfig cfg);
    ~JournalWriter();

    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;

    bool start();
    void stop();

    // Hot-path, non-blocking.  NO syscalls, NO locks, NO heap allocs.
    bool enqueue_record(RecordType type, std::uint16_t flags, std::uint64_t seqno,
                        const std::byte* payload, std::uint32_t payload_len) noexcept;

    JournalStats stats() const noexcept;

private:
    void io_thread_fn();

    JournalConfig cfg_;
    std::size_t   mask_ = 0;

    // ── Purpose-built SPSC ring ─────────────────────────────────────────
    // cache-line separated head/tail
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    std::unique_ptr<RingElement[]>       ring_;

    // ── Atomic counters (shared between producer & I/O thread) ──────────
    std::atomic<std::uint64_t> enqueued_     {0};
    std::atomic<std::uint64_t> written_      {0};
    std::atomic<std::uint64_t> dropped_      {0};
    std::atomic<std::uint64_t> io_errors_    {0};
    std::atomic<std::uint64_t> bytes_written_{0};
    std::atomic<bool>          degraded_io_  {false};

    // ── Lifecycle ───────────────────────────────────────────────────────
    std::atomic<bool> running_{false};

    // ── I/O thread private state ────────────────────────────────────────
    std::uint64_t              write_offset_ = 0;
    std::fstream               file_;
    std::unique_ptr<std::byte[]> scratch_;
    std::size_t                scratch_size_ = 0;
    std::thread                io_thread_;
};

} // namespace persist
