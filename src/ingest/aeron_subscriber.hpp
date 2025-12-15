#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <aeron/Aeron.h>

#include "core/exec_event.hpp"
#include "core/wire_exec_event.hpp"
#include "ingest/spsc_ring.hpp"

namespace ingest {

struct ThreadStats {
    std::size_t produced{0};
    std::size_t parse_failures{0};
    std::size_t drops{0};
};

using Ring = ingest::SpscRing<core::ExecEvent, 1u << 16>;

class AeronSubscriber {
public:
    AeronSubscriber(std::string channel,
                    std::int32_t stream_id,
                    Ring& ring,
                    ThreadStats& stats,
                    core::Source source,
                    std::shared_ptr<aeron::Aeron> client,
                    std::atomic<bool>& stop_flag) noexcept;

    void run();

private:
    std::string channel_;
    std::int32_t stream_id_;
    Ring& ring_;
    ThreadStats& stats_;
    core::Source source_;
    std::shared_ptr<aeron::Aeron> client_;
    std::atomic<bool>& stop_flag_;
};

} // namespace ingest
