#pragma once

#include <atomic>
#include <thread>

#include "core/order_state_store.hpp"
#include "ingest/spsc_ring.hpp"
#include "core/exec_event.hpp"

namespace core {

struct ReconCounters {
    std::size_t consumed_primary{0};
    std::size_t consumed_dropcopy{0};
    std::size_t parse_failures{0};
    std::size_t ring_drops{0};
    std::size_t store_overflow{0};
};

class Reconciler {
public:
    Reconciler(std::atomic<bool>& stop_flag,
               ingest::SpscRing<core::ExecEvent, 1u << 16>& primary,
               ingest::SpscRing<core::ExecEvent, 1u << 16>& dropcopy,
               OrderStateStore& store,
               ReconCounters& counters) noexcept;

    void run();

private:
    std::atomic<bool>& stop_flag_;
    ingest::SpscRing<core::ExecEvent, 1u << 16>& primary_;
    ingest::SpscRing<core::ExecEvent, 1u << 16>& dropcopy_;
    OrderStateStore& store_;
    ReconCounters& counters_;
};

} // namespace core
