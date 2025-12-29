#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace util {

// Hot-path event definitions. Producers must avoid formatting and allocation.
struct HotEventHeader {
    std::uint64_t ts{0};
    std::uint32_t tid{0};
    std::uint16_t type{0};
    std::uint16_t size{0};
    std::uint32_t flags{0};
};

enum class HotEventType : std::uint16_t {
    SeqGap = 1,
    Divergence = 2,
    StoreOverflow = 3,
    RingDrop = 4,
    HashMismatch = 5,
    LatencySample = 6,
    StateAnomaly = 7,
    Transport = 8,
    MetricsSnapshot = 9,
};

struct HotEvent {
    HotEventHeader header{};
    union Payload {
        struct SeqGap {
            std::uint64_t expected;
            std::uint64_t observed;
            std::uint32_t stream;
            std::uint32_t pad;
        } seq_gap;
        struct Divergence {
            std::uint64_t order_key;
            std::int64_t expected_px;
            std::int64_t observed_px;
            std::uint32_t field;
            std::uint32_t flags;
        } divergence;
        struct StoreOverflow {
            std::uint32_t capacity;
            std::uint32_t attempted;
            std::uint32_t table;
            std::uint32_t pad;
        } store_overflow;
        struct RingDrop {
            std::uint32_t ring_id;
            std::uint32_t dropped;
            std::uint32_t reason;
            std::uint32_t pad;
        } ring_drop;
        struct HashMismatch {
            std::uint64_t expected;
            std::uint64_t observed;
            std::uint32_t snapshot_id;
            std::uint32_t pad;
        } hash_mismatch;
        struct LatencySample {
            std::uint64_t sample_ns;
            std::uint32_t tag;
            std::uint32_t pad;
        } latency;
        struct StateAnomaly {
            std::uint32_t state;
            std::uint32_t event;
            std::uint32_t pad0;
            std::uint32_t pad1;
        } state_anomaly;
        struct Transport {
            std::uint32_t link;
            std::uint32_t code;
            std::uint32_t pad0;
            std::uint32_t pad1;
        } transport;
        struct MetricsSnapshot {
            std::uint64_t metric0;
            std::uint64_t metric1;
            std::uint64_t metric2;
            std::uint64_t metric3;
        } metrics;
        std::uint8_t raw[96];
    } payload{};
};
static_assert(sizeof(HotEvent) <= 128, "HotEvent must remain compact");

// Single-producer/single-consumer bounded ring. Capacity must be power of two.
template <typename T>
class SpscRing {
public:
    SpscRing() = default;

    bool init(std::size_t capacity_pow2) {
        if (capacity_pow2 < 2 || (capacity_pow2 & (capacity_pow2 - 1)) != 0) return false;
        capacity_ = capacity_pow2;
        mask_ = capacity_pow2 - 1;
        buffer_ = std::make_unique<T[]>(capacity_pow2);
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        return true;
    }

    bool try_push(const T& value) noexcept {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= capacity_) {
            return false; // full
        }
        buffer_[head & mask_] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept {
        const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            return false; // empty
        }
        out = buffer_[tail & mask_];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    std::size_t available() const noexcept {
        const std::size_t head = head_.load(std::memory_order_acquire);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        return head - tail;
    }

    std::size_t capacity() const noexcept { return capacity_; }

private:
    alignas(64) std::atomic<std::uint64_t> head_{0};
    alignas(64) std::atomic<std::uint64_t> tail_{0};
    std::size_t capacity_{0};
    std::size_t mask_{0};
    std::unique_ptr<T[]> buffer_{};
};

class HotLogger {
public:
    struct Config {
        std::size_t ring_size_pow2{1u << 10};
        std::size_t max_rings{64};
        bool use_rdtsc{false};
        std::string file_path{}; // empty -> stderr
        std::size_t flush_every{256};
        bool flush_on_warn{true};
        std::uint64_t consumer_sleep_ns{50'000};
    };

    HotLogger();
    ~HotLogger();

    HotLogger(const HotLogger&) = delete;
    HotLogger& operator=(const HotLogger&) = delete;

    bool start(const Config& cfg) noexcept;
    void stop() noexcept;

    bool emit(const HotEvent& ev) noexcept;

    // Helpers for common event families. These avoid formatting on the producer thread.
    bool emit_seq_gap(std::uint32_t stream, std::uint64_t expected, std::uint64_t observed, std::uint32_t flags = 0) noexcept;
    bool emit_divergence(std::uint64_t order_key, std::int64_t expected_px, std::int64_t observed_px, std::uint32_t field, std::uint32_t flags = 0) noexcept;
    bool emit_store_overflow(std::uint32_t table, std::uint32_t capacity, std::uint32_t attempted) noexcept;
    bool emit_ring_drop(std::uint32_t ring_id, std::uint32_t reason, std::uint32_t dropped) noexcept;
    bool emit_latency_sample(std::uint32_t tag, std::uint64_t ns) noexcept;

    std::uint64_t dropped() const noexcept { return global_dropped_.load(std::memory_order_relaxed); }

private:
    struct ProducerRing {
        SpscRing<HotEvent> ring{};
        std::atomic<std::uint64_t> dropped{0};
        std::uint32_t tid{0};
        bool active{false};
    };

    ProducerRing* ensure_ring() noexcept;
    std::uint64_t now_ticks() const noexcept;
    void consumer_loop() noexcept;
    void write_event(const HotEvent& ev) noexcept;

    Config config_{};
    std::atomic<bool> running_{false};
    std::atomic<std::uint32_t> next_tid_{1};
    std::thread consumer_{};

    FILE* sink_{stderr};
    bool owns_sink_{false};

    std::mutex registry_mutex_;
    std::vector<std::unique_ptr<ProducerRing>> rings_;
    std::vector<ProducerRing*> ring_view_;

    std::atomic<std::uint64_t> global_dropped_{0};
};

HotLogger& hot_logger() noexcept;
bool init_hot_logger(const HotLogger::Config& cfg) noexcept;
void shutdown_hot_logger() noexcept;

// Macros for hot-path structured events (allocation-free, formatting deferred to consumer)
#define HOT_SEQ_GAP(STREAM, EXPECTED, OBSERVED) ::util::hot_logger().emit_seq_gap((STREAM), (EXPECTED), (OBSERVED))
#define HOT_DIVERGENCE(KEY, EXP, OBS, FIELD, FLAGS) ::util::hot_logger().emit_divergence((KEY), (EXP), (OBS), (FIELD), (FLAGS))
#define HOT_STORE_OVERFLOW(TABLE, CAP, ATTEMPT) ::util::hot_logger().emit_store_overflow((TABLE), (CAP), (ATTEMPT))
#define HOT_RING_DROP(RING_ID, REASON, DROPPED) ::util::hot_logger().emit_ring_drop((RING_ID), (REASON), (DROPPED))
#define HOT_LATENCY(TAG, NS) ::util::hot_logger().emit_latency_sample((TAG), (NS))

} // namespace util

// Warm path formatted logging remains available via util::AsyncLogger (see async_log.hpp).

