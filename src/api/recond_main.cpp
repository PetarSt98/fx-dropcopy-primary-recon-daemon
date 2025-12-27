#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>
#include <cstdlib>

#include <Aeron.h>

#include "core/reconciler.hpp"
#include "core/order_state_store.hpp"
#include "ingest/aeron_subscriber.hpp"
#include "util/arena.hpp"
#include "util/async_log.hpp"

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <primary_channel> <primary_stream_id> <dropcopy_channel> <dropcopy_stream_id>" << std::endl;
        return 1;
    }

    util::AsyncLogger::Config hot_cfg{};
    hot_cfg.capacity_pow2 = 1u << 15;
    hot_cfg.use_rdtsc = true;
    if (!init_hot_logger(hot_cfg)) {
        LOG_SLOW_ERROR("Failed to start async logger for fx_exec_recond");
    }

    const std::string primary_channel = argv[1];
    const std::int32_t primary_stream = static_cast<std::int32_t>(std::stoi(argv[2]));
    const std::string dropcopy_channel = argv[3];
    const std::int32_t dropcopy_stream = static_cast<std::int32_t>(std::stoi(argv[4]));

    ingest::Ring primary_ring;
    ingest::Ring dropcopy_ring;
    core::DivergenceRing divergence_ring;
    core::SequenceGapRing seq_gap_ring;

    ingest::ThreadStats primary_stats;
    ingest::ThreadStats dropcopy_stats;
    core::ReconCounters counters;
    std::atomic<bool> stop_flag{false};
    util::Arena arena(util::Arena::default_capacity_bytes);
    constexpr std::size_t order_capacity_hint = 1u << 16;
    core::OrderStateStore store(arena, order_capacity_hint);

    aeron::Context context;
    auto client = aeron::Aeron::connect(context);

    core::Reconciler recon(stop_flag, primary_ring, dropcopy_ring, store, counters, divergence_ring, seq_gap_ring);

    ingest::AeronSubscriber primary_sub(primary_channel, primary_stream, primary_ring, primary_stats,
                                        core::Source::Primary, client, stop_flag);
    ingest::AeronSubscriber dropcopy_sub(dropcopy_channel, dropcopy_stream, dropcopy_ring, dropcopy_stats,
                                         core::Source::DropCopy, client, stop_flag);

    LOG_SLOW_INFO("Starting fx_exec_recond primary=%s stream=%d dropcopy=%s stream=%d",
                  primary_channel.c_str(), primary_stream, dropcopy_channel.c_str(), dropcopy_stream);

    std::thread primary_thread([&] { primary_sub.run(); });
    std::thread dropcopy_thread([&] { dropcopy_sub.run(); });
    std::thread recon_thread([&] { recon.run(); });

    const char* duration_env = std::getenv("RECOND_RUN_MS");
    if (duration_env) {
        const auto duration_ms = std::chrono::milliseconds{std::strtoul(duration_env, nullptr, 10)};
        LOG_SLOW_INFO("fx_exec_recond running for %llu ms before shutdown.",
                      static_cast<unsigned long long>(duration_ms.count()));
        std::this_thread::sleep_for(duration_ms);
    } else {
        LOG_SLOW_INFO("fx_exec_recond running. Press Enter to exit.");
        std::cin.get();
    }
    stop_flag.store(true, std::memory_order_release);

    primary_thread.join();
    dropcopy_thread.join();
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

    shutdown_hot_logger();

    return 0;
}
