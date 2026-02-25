#include "persist/journal_writer.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace persist {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

JournalWriter::JournalWriter(JournalConfig cfg)
    : cfg_(std::move(cfg))
{
}

JournalWriter::~JournalWriter() {
    stop();
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------

bool JournalWriter::start() {
    // Idempotent: if already running, succeed silently.
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }

    // Validate config
    const std::size_t cap = cfg_.ring_capacity_pow2;
    if (cap == 0 || (cap & (cap - 1)) != 0) {
        return false; // not power-of-two
    }

    // Allocate ring buffer
    try {
        ring_buf_ = std::make_unique<RingElement[]>(cap);
    } catch (...) {
        return false;
    }
    ring_mask_ = cap - 1;
    ring_head_.store(0, std::memory_order_relaxed);
    ring_tail_.store(0, std::memory_order_relaxed);

    // Allocate encode scratch buffer (header + max payload + padding)
    encode_buf_size_ = kHeaderSize + kMaxRingPayloadBytes + 8; // 8 for alignment padding
    try {
        encode_buf_ = std::make_unique<std::byte[]>(encode_buf_size_);
    } catch (...) {
        return false;
    }

    // Reset stats
    stat_enqueued_.store(0, std::memory_order_relaxed);
    stat_written_.store(0, std::memory_order_relaxed);
    stat_dropped_.store(0, std::memory_order_relaxed);
    stat_io_errors_.store(0, std::memory_order_relaxed);
    stat_bytes_written_.store(0, std::memory_order_relaxed);
    stat_degraded_io_.store(false, std::memory_order_relaxed);

    // Open file (truncate for v2.0 fresh-start policy)
    file_.open(cfg_.path, std::ios::binary | std::ios::trunc);
    if (!file_.is_open()) {
        return false;
    }
    write_offset_ = 0;

    // Preallocate
    if (!preallocate_file()) {
        file_.close();
        return false;
    }

    // Seek back to beginning for sequential writes
    file_.seekp(0, std::ios::beg);

    // Launch background thread
    running_.store(true, std::memory_order_release);
    try {
        consumer_ = std::thread([this] { consumer_loop(); });
    } catch (...) {
        running_.store(false, std::memory_order_release);
        file_.close();
        return false;
    }

    return true;
}

void JournalWriter::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return; // already stopped (or never started)
    }
    if (consumer_.joinable()) {
        consumer_.join();
    }
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
    // Truncate preallocated file to actual data size on clean shutdown.
    if (write_offset_ > 0) {
        try {
            std::filesystem::resize_file(cfg_.path, write_offset_);
        } catch (...) {
            // Best-effort; file is still valid up to write_offset_.
        }
    }
}

// ---------------------------------------------------------------------------
// enqueue_record — hot path (no syscalls, no locks, no heap allocs)
// ---------------------------------------------------------------------------

bool JournalWriter::enqueue_record(
    RecordType        type,
    std::uint16_t     flags,
    std::uint64_t     seqno,
    const std::byte*  payload,
    std::uint32_t     payload_len
) noexcept {
    // Must be running
    if (!running_.load(std::memory_order_acquire)) {
        return false;
    }

    // Payload too large for ring element — drop
    if (payload_len > kMaxRingPayloadBytes) {
        stat_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Build ring element on stack
    RingElement elem{};
    elem.type        = type;
    elem.flags       = flags;
    elem.seqno       = seqno;
    elem.payload_len = payload_len;
    if (payload_len > 0 && payload != nullptr) {
        std::memcpy(elem.payload, payload, payload_len);
    }

    // Try to push into SPSC ring (single producer)
    const auto head = ring_head_.load(std::memory_order_relaxed);
    const auto next_head = (head + 1) & ring_mask_;
    if (next_head == ring_tail_.load(std::memory_order_acquire)) {
        // Ring full — drop
        stat_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ring_buf_[head] = elem;
    ring_head_.store(next_head, std::memory_order_release);

    stat_enqueued_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---------------------------------------------------------------------------
// stats
// ---------------------------------------------------------------------------

JournalStats JournalWriter::stats() const noexcept {
    JournalStats s;
    s.enqueued      = stat_enqueued_.load(std::memory_order_relaxed);
    s.written       = stat_written_.load(std::memory_order_relaxed);
    s.dropped       = stat_dropped_.load(std::memory_order_relaxed);
    s.io_errors     = stat_io_errors_.load(std::memory_order_relaxed);
    s.bytes_written = stat_bytes_written_.load(std::memory_order_relaxed);
    s.degraded_io   = stat_degraded_io_.load(std::memory_order_relaxed);
    return s;
}

// ---------------------------------------------------------------------------
// Background thread
// ---------------------------------------------------------------------------

void JournalWriter::consumer_loop() noexcept {
    while (running_.load(std::memory_order_acquire) ||
           ring_tail_.load(std::memory_order_relaxed) != ring_head_.load(std::memory_order_acquire)) {
        const auto tail = ring_tail_.load(std::memory_order_relaxed);
        if (tail == ring_head_.load(std::memory_order_acquire)) {
            // Ring empty — sleep briefly
            if (cfg_.consumer_sleep_ns > 0) {
                std::this_thread::sleep_for(std::chrono::nanoseconds(cfg_.consumer_sleep_ns));
            } else {
                std::this_thread::yield();
            }
            continue;
        }

        // Pop element
        const RingElement& elem = ring_buf_[tail];
        write_element(elem);
        ring_tail_.store((tail + 1) & ring_mask_, std::memory_order_release);
    }

    // Final flush
    if (file_.is_open()) {
        file_.flush();
    }
}

void JournalWriter::write_element(const RingElement& elem) noexcept {
    // If degraded, keep draining but don't write (avoid blocking producer)
    if (stat_degraded_io_.load(std::memory_order_relaxed)) {
        return;
    }

    // Encode using FX-8001
    std::size_t encoded_bytes = 0;
    const bool ok = encode_record_v1(
        elem.type,
        elem.flags,
        elem.seqno,
        elem.payload,
        elem.payload_len,
        encode_buf_.get(),
        encode_buf_size_,
        encoded_bytes
    );

    if (!ok) {
        stat_io_errors_.fetch_add(1, std::memory_order_relaxed);
        stat_degraded_io_.store(true, std::memory_order_relaxed);
        return;
    }

    // Check segment capacity
    if (write_offset_ + encoded_bytes > cfg_.segment_bytes) {
        stat_io_errors_.fetch_add(1, std::memory_order_relaxed);
        stat_degraded_io_.store(true, std::memory_order_relaxed);
        return;
    }

    // Write to file at current offset
    file_.seekp(static_cast<std::streamoff>(write_offset_), std::ios::beg);
    file_.write(reinterpret_cast<const char*>(encode_buf_.get()),
                static_cast<std::streamsize>(encoded_bytes));

    if (!file_.good()) {
        stat_io_errors_.fetch_add(1, std::memory_order_relaxed);
        stat_degraded_io_.store(true, std::memory_order_relaxed);
        return;
    }

    write_offset_ += encoded_bytes;
    stat_written_.fetch_add(1, std::memory_order_relaxed);
    stat_bytes_written_.fetch_add(encoded_bytes, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Preallocation (cross-platform via std::filesystem::resize_file)
// ---------------------------------------------------------------------------

bool JournalWriter::preallocate_file() {
    try {
        // Flush any pending data before resizing
        file_.flush();
        std::filesystem::resize_file(cfg_.path, cfg_.segment_bytes);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace persist
