#include "persist/journal_writer.hpp"

#include <chrono>

namespace persist {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------
JournalWriter::JournalWriter(JournalConfig cfg)
    : cfg_(std::move(cfg)) {}

JournalWriter::~JournalWriter() {
    stop();
}

// ---------------------------------------------------------------------------
// start()
// ---------------------------------------------------------------------------
bool JournalWriter::start() {
    if (running_.load(std::memory_order_relaxed))
        return false; // already running

    // Validate capacity is power-of-two and non-zero
    if (cfg_.ring_capacity_pow2 == 0 ||
        (cfg_.ring_capacity_pow2 & (cfg_.ring_capacity_pow2 - 1)) != 0)
        return false;

    if (cfg_.max_payload_bytes > kJournalMaxPayload)
        return false;

    // Allocate ring storage once
    ring_ = std::make_unique<RingElement[]>(cfg_.ring_capacity_pow2);
    mask_ = cfg_.ring_capacity_pow2 - 1;
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);

    // Allocate scratch buffer: header + max_payload + alignment padding
    scratch_size_ = kHeaderSize + kJournalMaxPayload + 8;
    scratch_ = std::make_unique<std::byte[]>(scratch_size_);

    // Reset counters
    enqueued_.store(0, std::memory_order_relaxed);
    written_.store(0, std::memory_order_relaxed);
    dropped_.store(0, std::memory_order_relaxed);
    io_errors_.store(0, std::memory_order_relaxed);
    bytes_written_.store(0, std::memory_order_relaxed);
    degraded_io_.store(false, std::memory_order_relaxed);
    write_offset_ = 0;

    // Create / truncate file
    {
        std::ofstream ofs(cfg_.path, std::ios::binary | std::ios::trunc);
        if (!ofs)
            return false;
    }

    // Preallocate to segment_bytes
    {
        std::error_code ec;
        std::filesystem::resize_file(cfg_.path, cfg_.segment_bytes, ec);
        if (ec)
            return false;
    }

    // Open for sequential writing (in|out so no truncation on open)
    file_.open(cfg_.path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file_)
        return false;

    // Launch I/O thread
    running_.store(true, std::memory_order_release);
    io_thread_ = std::thread(&JournalWriter::io_thread_fn, this);

    return true;
}

// ---------------------------------------------------------------------------
// stop()
// ---------------------------------------------------------------------------
void JournalWriter::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel))
        return; // idempotent

    if (io_thread_.joinable())
        io_thread_.join();

    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }

    // Best-effort shrink to actual written data
    std::error_code ec;
    std::filesystem::resize_file(cfg_.path, write_offset_, ec);
    // ignore ec — best-effort
}

// ---------------------------------------------------------------------------
// enqueue_record  (hot-path — noexcept, no syscalls, no locks, no heap)
// ---------------------------------------------------------------------------
bool JournalWriter::enqueue_record(
    RecordType type, std::uint16_t flags, std::uint64_t seqno,
    const std::byte* payload, std::uint32_t payload_len) noexcept
{
    if (!running_.load(std::memory_order_relaxed))
        return false;

    if (payload_len > cfg_.max_payload_bytes) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // --- SPSC producer ---
    const auto h    = head_.load(std::memory_order_relaxed);
    const auto next = (h + 1) & mask_;
    if (next == tail_.load(std::memory_order_acquire)) {
        // ring full — drop, never block
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Write directly into ring slot (no stack temp, no zero-init)
    auto& slot       = ring_[h];
    slot.type        = type;
    slot.flags       = flags;
    slot.seqno       = seqno;
    slot.payload_len = payload_len;
    if (payload_len > 0)
        std::memcpy(slot.payload, payload, payload_len);

    // Publish
    head_.store(next, std::memory_order_release);
    enqueued_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---------------------------------------------------------------------------
// stats()  — atomic snapshot
// ---------------------------------------------------------------------------
JournalStats JournalWriter::stats() const noexcept {
    return {
        enqueued_.load(std::memory_order_relaxed),
        written_.load(std::memory_order_relaxed),
        dropped_.load(std::memory_order_relaxed),
        io_errors_.load(std::memory_order_relaxed),
        bytes_written_.load(std::memory_order_relaxed),
        degraded_io_.load(std::memory_order_relaxed),
    };
}

// ---------------------------------------------------------------------------
// I/O thread
// ---------------------------------------------------------------------------
void JournalWriter::io_thread_fn() {
    const auto idle_dur = std::chrono::nanoseconds(cfg_.idle_sleep_ns);

    while (true) {
        const auto t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) {
            // Queue empty
            if (!running_.load(std::memory_order_acquire))
                break; // stop requested and queue drained
            std::this_thread::sleep_for(idle_dur);
            continue;
        }

        const auto& elem = ring_[t];

        if (!degraded_io_.load(std::memory_order_relaxed)) {
            std::size_t encoded_bytes = 0;
            const bool ok = encode_record_v1(
                elem.type, elem.flags, elem.seqno,
                elem.payload, elem.payload_len,
                scratch_.get(), scratch_size_,
                encoded_bytes);

            if (!ok) {
                degraded_io_.store(true, std::memory_order_relaxed);
                io_errors_.fetch_add(1, std::memory_order_relaxed);
            } else if (write_offset_ + encoded_bytes > cfg_.segment_bytes) {
                // Segment full
                degraded_io_.store(true, std::memory_order_relaxed);
                io_errors_.fetch_add(1, std::memory_order_relaxed);
            } else {
                file_.write(reinterpret_cast<const char*>(scratch_.get()),
                            static_cast<std::streamsize>(encoded_bytes));
                if (!file_.good()) {
                    degraded_io_.store(true, std::memory_order_relaxed);
                    io_errors_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    write_offset_ += encoded_bytes;
                    written_.fetch_add(1, std::memory_order_relaxed);
                    bytes_written_.fetch_add(encoded_bytes, std::memory_order_relaxed);
                }
            }
        }
        // else: degraded — drain but don't write

        // Advance tail (consume)
        tail_.store((t + 1) & mask_, std::memory_order_release);
    }
}

} // namespace persist
