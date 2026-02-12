#include <benchmark/benchmark.h>

#include <vector>
#include <cstdio>
#include <cstring>

#include "core/order_state.hpp"
#include "core/order_state_store.hpp"
#include "ingest/spsc_ring.hpp"
#include "util/arena.hpp"
#include "util/wheel_timer.hpp"
#include "core/exec_event.hpp"

// ============================================================================
// Hash Table Lookup Benchmarks
// ============================================================================

// Best case: First probe (key in first slot)
static void BM_HashTableLookup_FirstProbe(benchmark::State& state) {
    // Use smaller arena (1MB instead of 512MB) to reduce memory pressure
    util::Arena arena(1ULL * 1024ULL * 1024ULL);
    core::OrderStateStore store(arena, 1024);

    // Insert a single order to ensure first-probe hit
    core::ExecEvent ev{};
    ev.ord_status = core::OrdStatus::New;
    std::memcpy(ev.clord_id, "ORDER001", 8);
    ev.clord_id_len = 8;
    ev.cum_qty = 0;
    ev.price_micro = 1000000;

    auto* os = store.upsert(ev);
    const core::OrderKey key = core::make_order_key(ev);
    benchmark::DoNotOptimize(os);

    for (auto _ : state) {
        auto* found = store.find(key);
        benchmark::DoNotOptimize(found);
    }
}
BENCHMARK(BM_HashTableLookup_FirstProbe);

// Worst case: Maximum probe chain (simulate high collision)
static void BM_HashTableLookup_MaxProbe(benchmark::State& state) {
    // Use smaller arena (1MB instead of 512MB) to reduce memory pressure
    util::Arena arena(1ULL * 1024ULL * 1024ULL);
    core::OrderStateStore store(arena, 256);

    // Create intentional collisions by generating keys that map to the same bucket
    // Since hash(key) = key, we need keys with the same (key & (bucket_count-1))
    const std::size_t bucket_count = store.bucket_count();
    const std::size_t target_bucket = 42;  // Arbitrary bucket to collide in
    
    std::vector<core::OrderKey> keys;
    // Insert keys that all map to the same bucket, forcing linear probing
    for (int i = 0; i < 50; ++i) {
        core::ExecEvent ev{};
        ev.ord_status = core::OrdStatus::New;
        
        // Generate ClOrdID that will hash to target_bucket
        // We'll use sequential keys: target_bucket, target_bucket + bucket_count, etc.
        char buf[16];
        std::snprintf(buf, sizeof(buf), "KEY%05d", static_cast<int>(target_bucket + i * bucket_count));
        std::memcpy(ev.clord_id, buf, 8);
        ev.clord_id_len = 8;
        ev.cum_qty = 0;
        ev.price_micro = 1000000;

        auto* os = store.upsert(ev);
        if (os) {
            keys.push_back(core::make_order_key(ev));
        }
    }

    // Look up a key that requires maximum probing (last inserted key in the collision chain)
    const core::OrderKey target_key = keys.empty() ? 0 : keys.back();

    for (auto _ : state) {
        auto* found = store.find(target_key);
        benchmark::DoNotOptimize(found);
    }
}
BENCHMARK(BM_HashTableLookup_MaxProbe);

// ============================================================================
// Hash Table Upsert Benchmark
// ============================================================================

static void BM_HashTableUpsert(benchmark::State& state) {
    // Use smaller arena (2MB instead of 512MB) to reduce memory pressure
    util::Arena arena(2ULL * 1024ULL * 1024ULL);
    core::OrderStateStore store(arena, 2048);

    int counter = 0;
    for (auto _ : state) {
        core::ExecEvent ev{};
        ev.ord_status = core::OrdStatus::New;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "ORD%05d", counter++);
        std::memcpy(ev.clord_id, buf, 8);
        ev.clord_id_len = 8;
        ev.cum_qty = 0;
        ev.price_micro = 1000000;

        auto* os = store.upsert(ev);
        benchmark::DoNotOptimize(os);

        // Reset arena periodically to avoid overflow
        // Pause timing so reset cost isn't included in measurements
        if (counter % 1000 == 0) {
            state.PauseTiming();
            store.reset_epoch();
            counter = 0;
            state.ResumeTiming();
        }
    }
}
BENCHMARK(BM_HashTableUpsert);

// ============================================================================
// Arena Allocation Benchmark
// ============================================================================

static void BM_ArenaAllocate(benchmark::State& state) {
    // Use smaller arena (1MB instead of 512MB) to reduce memory pressure
    util::Arena arena(1ULL * 1024ULL * 1024ULL);
    int alloc_count = 0;

    for (auto _ : state) {
        void* ptr = arena.allocate(sizeof(core::OrderState), alignof(core::OrderState));
        benchmark::DoNotOptimize(ptr);

        // Reset arena periodically to avoid exhaustion
        if (++alloc_count % 10000 == 0) {
            arena.reset();
            alloc_count = 0;
        }
    }
}
BENCHMARK(BM_ArenaAllocate);

// ============================================================================
// Timer Wheel Schedule Benchmark
// ============================================================================

static void BM_TimerWheelSchedule(benchmark::State& state) {
    util::WheelTimer timer(0);

    core::OrderKey key = 12345;
    std::uint32_t generation = 0;
    
    // Use proper TSC-based stride to distribute entries across buckets
    const std::uint64_t tick_tsc = timer.tick_tsc();
    std::uint64_t base_deadline_tsc = tick_tsc;

    int counter = 0;
    for (auto _ : state) {
        // Schedule with deadlines spaced by tick_tsc to avoid bucket overflow
        std::uint64_t deadline = base_deadline_tsc + static_cast<std::uint64_t>(counter) * tick_tsc;
        bool scheduled = timer.schedule(key + counter, generation, deadline);
        benchmark::DoNotOptimize(scheduled);
        ++counter;

        // Reset timer periodically to avoid bucket overflow
        // Pause timing so reset cost isn't included in measurements
        if (counter % 5000 == 0) {
            state.PauseTiming();
            timer.reset(0);
            counter = 0;
            base_deadline_tsc = tick_tsc;
            state.ResumeTiming();
        }
    }
}
BENCHMARK(BM_TimerWheelSchedule);

// ============================================================================
// Mismatch Computation Benchmark
// ============================================================================

static void BM_ComputeMismatch(benchmark::State& state) {
    core::OrderState os{};
    os.key = 12345;
    os.seen_internal = true;
    os.seen_dropcopy = true;
    os.internal_status = core::OrdStatus::PartiallyFilled;
    os.dropcopy_status = core::OrdStatus::PartiallyFilled;
    os.internal_cum_qty = 100;
    os.dropcopy_cum_qty = 100;
    os.internal_avg_px = 1000000;
    os.dropcopy_avg_px = 1000000;

    for (auto _ : state) {
        core::MismatchMask mask = core::compute_mismatch(os);
        benchmark::DoNotOptimize(mask);
    }
}
BENCHMARK(BM_ComputeMismatch);

// ============================================================================
// SPSC Ring Push/Pop Benchmark
// ============================================================================

static void BM_SPSCRing_Push(benchmark::State& state) {
    ingest::SpscRing<core::ExecEvent, 1u << 16> ring;
    core::ExecEvent evt{};
    evt.ord_status = core::OrdStatus::New;
    std::memcpy(evt.clord_id, "ORDER001", 8);
    evt.clord_id_len = 8;
    int push_count = 0;

    for (auto _ : state) {
        bool pushed = ring.try_push(evt);
        benchmark::DoNotOptimize(pushed);

        // Pop periodically to avoid filling the ring, but do not time the pops
        if (++push_count % 1000 == 0) {
            state.PauseTiming();
            core::ExecEvent dummy;
            for (int i = 0; i < 1000; ++i) {
                ring.try_pop(dummy);
            }
            push_count = 0;
            state.ResumeTiming();
        }
    }
}
BENCHMARK(BM_SPSCRing_Push);

static void BM_SPSCRing_Pop(benchmark::State& state) {
    ingest::SpscRing<core::ExecEvent, 1u << 16> ring;
    core::ExecEvent evt{};
    evt.ord_status = core::OrdStatus::New;
    std::memcpy(evt.clord_id, "ORDER001", 8);
    evt.clord_id_len = 8;

    // Pre-fill ring with events
    for (std::size_t i = 0; i < 10000; ++i) {
        ring.try_push(evt);
    }

    int refill_counter = 0;
    for (auto _ : state) {
        core::ExecEvent out;
        bool popped = ring.try_pop(out);
        benchmark::DoNotOptimize(popped);
        benchmark::DoNotOptimize(out);

        // Refill periodically to avoid emptying, but do not include refill in timing
        if (++refill_counter % 1000 == 0) {
            state.PauseTiming();
            for (int i = 0; i < 1000; ++i) {
                ring.try_push(evt);
            }
            refill_counter = 0;
            state.ResumeTiming();
        }
    }
}
BENCHMARK(BM_SPSCRing_Pop);

static void BM_SPSCRing_PushPop(benchmark::State& state) {
    ingest::SpscRing<core::ExecEvent, 1u << 16> ring;
    core::ExecEvent evt{};
    evt.ord_status = core::OrdStatus::New;
    std::memcpy(evt.clord_id, "ORDER001", 8);
    evt.clord_id_len = 8;

    for (auto _ : state) {
        ring.try_push(evt);
        core::ExecEvent out;
        bool popped = ring.try_pop(out);
        benchmark::DoNotOptimize(popped);
        benchmark::DoNotOptimize(out);
    }
}
BENCHMARK(BM_SPSCRing_PushPop);
