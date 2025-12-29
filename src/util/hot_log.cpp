#include "util/hot_log.hpp"

#include <chrono>
#include <cstring>

#include "util/rdtsc.hpp"

namespace util {
namespace {
HotLogger* global_hot_logger() {
    static HotLogger logger;
    return &logger;
}

thread_local HotLogger::ProducerRing* tls_ring = nullptr;
} // namespace

HotLogger::HotLogger() = default;
HotLogger::~HotLogger() { stop(); }

bool HotLogger::start(const Config& cfg) noexcept {
    if (running_.load(std::memory_order_acquire)) {
        return true;
    }

    if (cfg.ring_size_pow2 < 2 || (cfg.ring_size_pow2 & (cfg.ring_size_pow2 - 1)) != 0) {
        return false;
    }
    if (cfg.max_rings == 0) {
        return false;
    }

    config_ = cfg;
    global_dropped_.store(0, std::memory_order_relaxed);
    next_tid_.store(1, std::memory_order_relaxed);
    if (!config_.file_path.empty()) {
        sink_ = std::fopen(config_.file_path.c_str(), "a");
        if (!sink_) {
            return false;
        }
        owns_sink_ = true;
    } else {
        sink_ = stderr;
        owns_sink_ = false;
    }

    running_.store(true, std::memory_order_release);
    try {
        consumer_ = std::thread([this] { consumer_loop(); });
    } catch (...) {
        running_.store(false, std::memory_order_release);
        if (owns_sink_ && sink_) {
            std::fclose(sink_);
        }
        sink_ = stderr;
        owns_sink_ = false;
        return false;
    }
    return true;
}

void HotLogger::stop() noexcept {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (consumer_.joinable()) {
        consumer_.join();
    }
    std::lock_guard<std::mutex> lk(registry_mutex_);
    // Keep rings allocated to avoid stale TLS dereferences; logging is disabled via running_.
    if (owns_sink_ && sink_) {
        std::fclose(sink_);
    }
    sink_ = stderr;
    owns_sink_ = false;
}

std::uint64_t HotLogger::now_ticks() const noexcept {
    if (config_.use_rdtsc) {
        return ::util::rdtsc();
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

HotLogger::ProducerRing* HotLogger::ensure_ring() noexcept {
    if (!running_.load(std::memory_order_acquire)) {
        return nullptr;
    }
    if (tls_ring) {
        return tls_ring;
    }
    std::lock_guard<std::mutex> lk(registry_mutex_);
    if (!running_.load(std::memory_order_acquire)) {
        return nullptr;
    }
    if (rings_.size() >= config_.max_rings) {
        return nullptr;
    }
    auto ring = std::make_unique<ProducerRing>();
    if (!ring->ring.init(config_.ring_size_pow2)) {
        return nullptr;
    }
    ring->tid = next_tid_.fetch_add(1, std::memory_order_relaxed);
    tls_ring = ring.get();
    rings_.push_back(std::move(ring));
    ring_view_.push_back(tls_ring);
    return tls_ring;
}

bool HotLogger::emit(const HotEvent& ev) noexcept {
    ProducerRing* ring = ensure_ring();
    if (!ring) {
        return false;
    }
    HotEvent event = ev;
    event.header.tid = ring->tid;
    event.header.ts = now_ticks();
    event.header.size = sizeof(event);
    if (!ring->ring.try_push(event)) {
        ring->dropped.fetch_add(1, std::memory_order_relaxed);
        global_dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool HotLogger::emit_seq_gap(std::uint32_t stream, std::uint64_t expected, std::uint64_t observed, std::uint32_t flags) noexcept {
    HotEvent ev{};
    ev.header.type = static_cast<std::uint16_t>(HotEventType::SeqGap);
    ev.header.flags = flags;
    ev.payload.seq_gap.expected = expected;
    ev.payload.seq_gap.observed = observed;
    ev.payload.seq_gap.stream = stream;
    return emit(ev);
}

bool HotLogger::emit_divergence(std::uint64_t order_key, std::int64_t expected_px, std::int64_t observed_px, std::uint32_t field, std::uint32_t flags) noexcept {
    HotEvent ev{};
    ev.header.type = static_cast<std::uint16_t>(HotEventType::Divergence);
    ev.header.flags = flags;
    ev.payload.divergence.order_key = order_key;
    ev.payload.divergence.expected_px = expected_px;
    ev.payload.divergence.observed_px = observed_px;
    ev.payload.divergence.field = field;
    ev.payload.divergence.flags = flags;
    return emit(ev);
}

bool HotLogger::emit_store_overflow(std::uint32_t table, std::uint32_t capacity, std::uint32_t attempted) noexcept {
    HotEvent ev{};
    ev.header.type = static_cast<std::uint16_t>(HotEventType::StoreOverflow);
    ev.header.flags = HOT_FLAG_FLUSH;
    ev.payload.store_overflow.table = table;
    ev.payload.store_overflow.capacity = capacity;
    ev.payload.store_overflow.attempted = attempted;
    return emit(ev);
}

bool HotLogger::emit_ring_drop(std::uint32_t ring_id, std::uint32_t reason, std::uint32_t dropped) noexcept {
    HotEvent ev{};
    ev.header.type = static_cast<std::uint16_t>(HotEventType::RingDrop);
    ev.header.flags = HOT_FLAG_FLUSH;
    ev.payload.ring_drop.ring_id = ring_id;
    ev.payload.ring_drop.reason = reason;
    ev.payload.ring_drop.dropped = dropped;
    return emit(ev);
}

bool HotLogger::emit_latency_sample(std::uint32_t tag, std::uint64_t ns) noexcept {
    HotEvent ev{};
    ev.header.type = static_cast<std::uint16_t>(HotEventType::LatencySample);
    ev.payload.latency.tag = tag;
    ev.payload.latency.sample_ns = ns;
    return emit(ev);
}

bool HotLogger::emit_state_anomaly(std::uint64_t key, std::uint32_t state, std::uint32_t event, std::uint32_t flags) noexcept {
    HotEvent ev{};
    ev.header.type = static_cast<std::uint16_t>(HotEventType::StateAnomaly);
    ev.header.flags = flags;
    ev.payload.state_anomaly.key = key;
    ev.payload.state_anomaly.state = state;
    ev.payload.state_anomaly.event = event;
    return emit(ev);
}

void HotLogger::write_event(const HotEvent& ev) noexcept {
    FILE* sink = sink_;
    if (!sink) return;
    const auto type = static_cast<HotEventType>(ev.header.type);
    switch (type) {
    case HotEventType::SeqGap:
        std::fprintf(sink, "[HOT][SEQ_GAP][tid=%u][stream=%u exp=%llu obs=%llu flags=%u]\n", ev.header.tid,
                     ev.payload.seq_gap.stream,
                     static_cast<unsigned long long>(ev.payload.seq_gap.expected),
                     static_cast<unsigned long long>(ev.payload.seq_gap.observed), ev.header.flags);
        break;
    case HotEventType::Divergence:
        std::fprintf(sink, "[HOT][DIVERGENCE][tid=%u][key=%llu exp=%lld obs=%lld field=%u flags=%u]\n", ev.header.tid,
                     static_cast<unsigned long long>(ev.payload.divergence.order_key),
                     static_cast<long long>(ev.payload.divergence.expected_px),
                     static_cast<long long>(ev.payload.divergence.observed_px), ev.payload.divergence.field,
                     ev.payload.divergence.flags);
        break;
    case HotEventType::StoreOverflow:
        std::fprintf(sink, "[HOT][STORE_OVERFLOW][tid=%u][table=%u cap=%u attempt=%u]\n", ev.header.tid,
                     ev.payload.store_overflow.table, ev.payload.store_overflow.capacity,
                     ev.payload.store_overflow.attempted);
        break;
    case HotEventType::RingDrop:
        std::fprintf(sink, "[HOT][RING_DROP][tid=%u][ring=%u reason=%u dropped=%u]\n", ev.header.tid,
                     ev.payload.ring_drop.ring_id, ev.payload.ring_drop.reason, ev.payload.ring_drop.dropped);
        break;
    case HotEventType::HashMismatch:
        std::fprintf(sink, "[HOT][HASH_MISMATCH][tid=%u][snap=%u exp=%llu obs=%llu]\n", ev.header.tid,
                     ev.payload.hash_mismatch.snapshot_id,
                     static_cast<unsigned long long>(ev.payload.hash_mismatch.expected),
                     static_cast<unsigned long long>(ev.payload.hash_mismatch.observed));
        break;
    case HotEventType::LatencySample:
        std::fprintf(sink, "[HOT][LATENCY][tid=%u][tag=%u ns=%llu]\n", ev.header.tid, ev.payload.latency.tag,
                     static_cast<unsigned long long>(ev.payload.latency.sample_ns));
        break;
    case HotEventType::StateAnomaly:
        std::fprintf(sink, "[HOT][STATE_ANOM][tid=%u][key=%llu state=%u event=%u]\n", ev.header.tid,
                     static_cast<unsigned long long>(ev.payload.state_anomaly.key), ev.payload.state_anomaly.state,
                     ev.payload.state_anomaly.event);
        break;
    case HotEventType::Transport:
        std::fprintf(sink, "[HOT][TRANSPORT][tid=%u][link=%u code=%u]\n", ev.header.tid, ev.payload.transport.link,
                     ev.payload.transport.code);
        break;
    case HotEventType::MetricsSnapshot:
        std::fprintf(sink, "[HOT][METRICS][tid=%u][m0=%llu m1=%llu m2=%llu m3=%llu]\n", ev.header.tid,
                     static_cast<unsigned long long>(ev.payload.metrics.metric0),
                     static_cast<unsigned long long>(ev.payload.metrics.metric1),
                     static_cast<unsigned long long>(ev.payload.metrics.metric2),
                     static_cast<unsigned long long>(ev.payload.metrics.metric3));
        break;
    }
}

void HotLogger::consumer_loop() noexcept {
    std::size_t since_flush = 0;
    std::uint32_t idle_spins = 0;
    std::vector<ProducerRing*> local_rings;
    local_rings.reserve(config_.max_rings);
    while (running_.load(std::memory_order_acquire)) {
        bool had = false;
        {
            std::lock_guard<std::mutex> lk(registry_mutex_);
            local_rings = ring_view_;
        }
        for (auto* pr : local_rings) {
            if (!pr) continue;
            HotEvent ev{};
            while (pr->ring.try_pop(ev)) {
                had = true;
                idle_spins = 0;
                write_event(ev);
                ++since_flush;
                if ((config_.flush_on_warn && (ev.header.flags & HOT_FLAG_FLUSH) != 0) ||
                    (config_.flush_every > 0 && since_flush >= config_.flush_every)) {
                    std::fflush(sink_);
                    since_flush = 0;
                }
            }
        }
        if (!had) {
            if (idle_spins < 256) {
                ++idle_spins;
                std::this_thread::yield();
            } else if (config_.consumer_sleep_ns > 0) {
                idle_spins = 0;
                std::this_thread::sleep_for(std::chrono::nanoseconds(config_.consumer_sleep_ns));
            }
        }
    }
    {
        std::lock_guard<std::mutex> lk(registry_mutex_);
        local_rings = ring_view_;
    }
    for (auto* pr : local_rings) {
        if (!pr) continue;
        HotEvent ev{};
        while (pr->ring.try_pop(ev)) {
            write_event(ev);
        }
    }
    std::fflush(sink_);
}

HotLogger& hot_logger() noexcept { return *global_hot_logger(); }

bool init_hot_logger(const HotLogger::Config& cfg) noexcept { return hot_logger().start(cfg); }

void shutdown_hot_logger() noexcept { hot_logger().stop(); }

} // namespace util

