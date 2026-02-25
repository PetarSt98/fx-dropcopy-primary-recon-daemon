// FX-8002: Async Journal Writer — implementation.

#include "persist/journal_writer.hpp"

#include <chrono>

namespace persist {

// ── ctor / dtor ─────────────────────────────────────────────────────────────

JournalWriter::JournalWriter(JournalConfig cfg)
    : config_(std::move(cfg))
    , ring_mask_(config_.ring_capacity_pow2 - 1)
{}

JournalWriter::~JournalWriter() { stop(); }

// ── start ───────────────────────────────────────────────────────────────────

bool JournalWriter::start() {
    if (running_.load(std::memory_order_acquire))
        return true; // idempotent

    // Validate config.
    const auto cap = config_.ring_capacity_pow2;
    if (cap < 2 || (cap & (cap - 1)) != 0)
        return false;
    ring_mask_ = cap - 1;

    // Allocate ring & scratch (once; no per-record heap churn).
    ring_buf_  = std::make_unique<RingElement[]>(cap);
    ring_head_.store(0, std::memory_order_relaxed);
    ring_tail_.store(0, std::memory_order_relaxed);

    const std::size_t scratch_size = kHeaderSize + kMaxRingPayload + kAlignment;
    scratch_ = std::make_unique<std::byte[]>(scratch_size);

    // Truncate file (v2.0 policy: fresh journal each run).
    {
        std::ofstream ofs(config_.path,
                          std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;
    }

    // Preallocate to segment_bytes (cross-platform).
    {
        std::error_code ec;
        std::filesystem::resize_file(config_.path, config_.segment_bytes, ec);
        // Best-effort; ignore errors.
    }

    // Open for sequential writing (in|out avoids re-truncation).
    file_.open(config_.path, std::ios::binary | std::ios::in | std::ios::out);
    if (!file_.is_open()) return false;
    // Position is at 0.  All subsequent writes are sequential — no seekp().

    write_offset_ = 0;

    // Reset stats.
    stat_enqueued_.store(0, std::memory_order_relaxed);
    stat_written_.store(0, std::memory_order_relaxed);
    stat_dropped_.store(0, std::memory_order_relaxed);
    stat_io_errors_.store(0, std::memory_order_relaxed);
    stat_bytes_written_.store(0, std::memory_order_relaxed);
    stat_degraded_io_.store(false, std::memory_order_relaxed);

    stop_flag_.store(false, std::memory_order_relaxed);
    running_.store(true, std::memory_order_release);

    try {
        bg_thread_ = std::thread([this] { background_loop(); });
    } catch (...) {
        running_.store(false, std::memory_order_release);
        file_.close();
        return false;
    }
    return true;
}

// ── stop ────────────────────────────────────────────────────────────────────

void JournalWriter::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel))
        return; // not running

    stop_flag_.store(true, std::memory_order_release);

    if (bg_thread_.joinable())
        bg_thread_.join();

    file_.flush();
    file_.close();
}

// ── enqueue (hot path) ─────────────────────────────────────────────────────

bool JournalWriter::enqueue_record(RecordType       type,
                                   std::uint16_t    flags,
                                   std::uint64_t    seqno,
                                   const std::byte* payload,
                                   std::uint32_t    payload_len) noexcept {
    if (!running_.load(std::memory_order_acquire))
        return false;

    if (payload_len > kMaxRingPayload) {
        stat_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // SPSC producer: write directly into ring slot (no stack temp, no zero-init).
    const auto head = ring_head_.load(std::memory_order_relaxed);
    const auto next = (head + 1) & ring_mask_;

    if (next == ring_tail_.load(std::memory_order_acquire)) {
        // Ring full → drop.
        stat_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    RingElement& slot = ring_buf_[head];
    slot.type        = type;
    slot.flags       = flags;
    slot.seqno       = seqno;
    slot.payload_len = payload_len;
    if (payload_len > 0 && payload)
        std::memcpy(slot.payload, payload, payload_len);

    ring_head_.store(next, std::memory_order_release);
    stat_enqueued_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ── stats ───────────────────────────────────────────────────────────────────

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

// ── background thread ───────────────────────────────────────────────────────

void JournalWriter::background_loop() {
    for (;;) {
        const auto tail = ring_tail_.load(std::memory_order_relaxed);
        if (tail == ring_head_.load(std::memory_order_acquire)) {
            // Ring empty.
            if (stop_flag_.load(std::memory_order_acquire))
                break; // drain complete
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }

        const RingElement& slot = ring_buf_[tail];

        // If degraded, keep draining to avoid blocking producer but skip I/O.
        if (stat_degraded_io_.load(std::memory_order_relaxed)) {
            ring_tail_.store((tail + 1) & ring_mask_,
                             std::memory_order_release);
            continue;
        }

        // Encode from ring slot into scratch buffer.
        const auto enc = encode_record_v1(
            scratch_.get(), slot.type, slot.flags, slot.seqno,
            slot.payload, slot.payload_len);

        // Release ring slot (all data copied to scratch).
        ring_tail_.store((tail + 1) & ring_mask_,
                         std::memory_order_release);

        if (!enc.ok) {
            stat_io_errors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // Segment capacity check.
        if (write_offset_ + enc.total_bytes > config_.segment_bytes) {
            stat_degraded_io_.store(true, std::memory_order_relaxed);
            stat_io_errors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        // Sequential write — no seekp() per record.
        file_.write(reinterpret_cast<const char*>(scratch_.get()),
                    static_cast<std::streamsize>(enc.total_bytes));

        if (!file_.good()) {
            stat_degraded_io_.store(true, std::memory_order_relaxed);
            stat_io_errors_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        write_offset_ += enc.total_bytes;
        stat_written_.fetch_add(1, std::memory_order_relaxed);
        stat_bytes_written_.fetch_add(enc.total_bytes, std::memory_order_relaxed);
    }

    file_.flush();
}

} // namespace persist
