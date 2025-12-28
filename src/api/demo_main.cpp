#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "core/exec_event.hpp"
#include "core/reconciler.hpp"
#include "core/order_state_store.hpp"
#include "ingest/fix_parser.hpp"
#include "ingest/spsc_ring.hpp"
#include "util/arena.hpp"
#include "util/async_log.hpp"
#include "util/soh.hpp"

// SPSC rings are fixed-size (power of two). On push failure the caller drops and counts the
// message; no blocking or heap fallback in the hot path.
using Ring = ingest::SpscRing<core::ExecEvent, 1u << 16>;

struct ThreadStats {
    std::size_t produced{0};
    std::size_t parse_failures{0};
    std::size_t drops{0};
};

std::string make_exec_report(std::size_t seq, int64_t price_micro, int64_t qty) {
    std::string msg = "8=FIX.4.4|35=8|150=2|39=2|17=EXEC" + std::to_string(seq) +
                      "|11=CID" + std::to_string(seq) +
                      "|37=OID" + std::to_string(seq) +
                      "|31=" + std::to_string(price_micro) +
                      "|32=" + std::to_string(qty) +
                      "|14=" + std::to_string(qty) +
                      "|52=20240101000000000|60=20240101000000000|";
    return util::pipe_to_soh(msg);
}

// Ingest threads never throw in the hot path; they simply count failures and return on stop.
void ingest_thread(std::atomic<bool>& stop_flag, Ring& ring, ThreadStats& stats, core::Source src) {
    std::size_t seq = 1;
    core::ExecEvent evt{};
    while (!stop_flag.load(std::memory_order_acquire)) {
        const std::string msg = make_exec_report(seq, 1000000 + static_cast<int64_t>(seq), 100 + (seq % 10));
        if (ingest::parse_exec_report(msg.data(), msg.size(), evt) != ingest::ParseResult::Ok) {
            ++stats.parse_failures;
        } else {
            evt.source = src;
            evt.seq_num = seq;
            if (!ring.try_push(evt)) {
                ++stats.drops;
            } else {
                ++stats.produced;
            }
        }
        ++seq;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

int main() {
    // Thread lifecycle: main sets up structures, launches ingest + reconciler threads, and on
    // shutdown sets stop_flag then joins in deterministic order (ingest before reconciler).
    util::AsyncLogger::Config hot_cfg{};
    hot_cfg.capacity_pow2 = 1u << 14;
    hot_cfg.use_rdtsc = true;
    if (!init_hot_logger(hot_cfg)) {
        LOG_SLOW_ERROR("Failed to start async logger for demo");
    }

    std::atomic<bool> stop_flag{false};
    Ring primary_ring;
    Ring dropcopy_ring;
    core::DivergenceRing divergence_ring;
    core::SequenceGapRing seq_gap_ring;

    ThreadStats primary_stats;
    ThreadStats dropcopy_stats;
    core::ReconCounters counters;
    util::Arena arena(util::Arena::default_capacity_bytes);
    constexpr std::size_t order_capacity_hint = 1u << 14;
    core::OrderStateStore store(arena, order_capacity_hint);

    core::Reconciler recon(stop_flag, primary_ring, dropcopy_ring, store, counters, divergence_ring, seq_gap_ring);

    std::thread primary([&] { ingest_thread(stop_flag, primary_ring, primary_stats, core::Source::Primary); });
    std::thread dropcopy([&] { ingest_thread(stop_flag, dropcopy_ring, dropcopy_stats, core::Source::DropCopy); });
    std::thread recon_thread([&] { recon.run(); });

    LOG_SLOW_INFO("Running demo for 2 seconds...");
    std::this_thread::sleep_for(std::chrono::seconds(2));
    stop_flag.store(true, std::memory_order_release);

    primary.join();
    dropcopy.join();
    recon_thread.join();

    LOG_SLOW_INFO("Primary produced=%zu drops=%zu parse_failures=%zu", primary_stats.produced, primary_stats.drops,
                  primary_stats.parse_failures);
    LOG_SLOW_INFO("DropCopy produced=%zu drops=%zu parse_failures=%zu", dropcopy_stats.produced, dropcopy_stats.drops,
                  dropcopy_stats.parse_failures);
    LOG_SLOW_INFO("Reconciler processed internal=%llu dropcopy=%llu divergences=%llu ring_drops=%llu",
                  static_cast<unsigned long long>(counters.internal_events),
                  static_cast<unsigned long long>(counters.dropcopy_events),
                  static_cast<unsigned long long>(counters.divergence_total),
                  static_cast<unsigned long long>(counters.divergence_ring_drops));

    util::shutdown_hot_logger();
    return 0;
}
