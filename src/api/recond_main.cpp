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

int main(int argc, char** argv) {
    if (argc < 5) {
        std::cerr << "Usage: " << argv[0]
                  << " <primary_channel> <primary_stream_id> <dropcopy_channel> <dropcopy_stream_id>" << std::endl;
        return 1;
    }

    const std::string primary_channel = argv[1];
    const std::int32_t primary_stream = static_cast<std::int32_t>(std::stoi(argv[2]));
    const std::string dropcopy_channel = argv[3];
    const std::int32_t dropcopy_stream = static_cast<std::int32_t>(std::stoi(argv[4]));

    ingest::Ring primary_ring;
    ingest::Ring dropcopy_ring;
    core::DivergenceRing divergence_ring;

    ingest::ThreadStats primary_stats;
    ingest::ThreadStats dropcopy_stats;
    core::ReconCounters counters;
    std::atomic<bool> stop_flag{false};
    util::Arena arena(util::Arena::default_capacity_bytes);
    constexpr std::size_t order_capacity_hint = 1u << 16;
    core::OrderStateStore store(arena, order_capacity_hint);

    aeron::Context context;
    auto client = aeron::Aeron::connect(context);

    core::Reconciler recon(stop_flag, primary_ring, dropcopy_ring, store, counters, divergence_ring);

    ingest::AeronSubscriber primary_sub(primary_channel, primary_stream, primary_ring, primary_stats,
                                        core::Source::Primary, client, stop_flag);
    ingest::AeronSubscriber dropcopy_sub(dropcopy_channel, dropcopy_stream, dropcopy_ring, dropcopy_stats,
                                         core::Source::DropCopy, client, stop_flag);

    std::thread primary_thread([&] { primary_sub.run(); });
    std::thread dropcopy_thread([&] { dropcopy_sub.run(); });
    std::thread recon_thread([&] { recon.run(); });

    const char* duration_env = std::getenv("RECOND_RUN_MS");
    if (duration_env) {
        const auto duration_ms = std::chrono::milliseconds{std::strtoul(duration_env, nullptr, 10)};
        std::cout << "fx_exec_recond running for " << duration_ms.count() << "ms before shutdown." << std::endl;
        std::this_thread::sleep_for(duration_ms);
    } else {
        std::cout << "fx_exec_recond running. Press Enter to exit." << std::endl;
        std::cin.get();
    }
    stop_flag.store(true, std::memory_order_release);

    primary_thread.join();
    dropcopy_thread.join();
    recon_thread.join();

    std::cout << "Primary produced: " << primary_stats.produced << " drops: " << primary_stats.drops
              << " parse_failures: " << primary_stats.parse_failures << "\n";
    std::cout << "DropCopy produced: " << dropcopy_stats.produced << " drops: " << dropcopy_stats.drops
              << " parse_failures: " << dropcopy_stats.parse_failures << "\n";
    std::cout << "Reconciler processed internal: " << counters.internal_events
              << " dropcopy: " << counters.dropcopy_events << "\n";
    std::cout << "Divergences total: " << counters.divergence_total
              << " ring_drops: " << counters.divergence_ring_drops << "\n";

    return 0;
}
