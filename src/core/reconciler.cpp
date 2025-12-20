#include "core/reconciler.hpp"

#include <chrono>
#include <thread>

namespace core {

Reconciler::Reconciler(std::atomic<bool>& stop_flag,
                       ingest::SpscRing<core::ExecEvent, 1u << 16>& primary,
                       ingest::SpscRing<core::ExecEvent, 1u << 16>& dropcopy,
                       OrderStateStore& store,
                       ReconCounters& counters) noexcept
    : stop_flag_(stop_flag), primary_(primary), dropcopy_(dropcopy), store_(store), counters_(counters) {}

void Reconciler::run() {
    ExecEvent evt{};
    std::uint32_t backoff = 0;
    while (!stop_flag_.load(std::memory_order_acquire)) {
        bool consumed = false;
        if (primary_.try_pop(evt)) {
            ++counters_.consumed_primary;
            if (!store_.upsert(evt)) {
                ++counters_.store_overflow;
            }
            consumed = true;
        }
        if (dropcopy_.try_pop(evt)) {
            ++counters_.consumed_dropcopy;
            if (!store_.upsert(evt)) {
                ++counters_.store_overflow;
            }
            consumed = true;
        }
        if (!consumed) {
            if (backoff < 16) {
                ++backoff;
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        } else {
            backoff = 0;
        }
    }
}

} // namespace core
