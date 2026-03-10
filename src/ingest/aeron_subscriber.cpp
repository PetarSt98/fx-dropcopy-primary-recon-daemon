#include "ingest/aeron_subscriber.hpp"

#include <cstring>
#include <thread>

#include "util/cpu_relax.hpp"
#include "util/rdtsc.hpp"
#include "util/perf_macros.hpp"

namespace ingest {

namespace {

inline void inc_relaxed(std::atomic<std::size_t>& counter) noexcept {
    counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

} // namespace

AeronSubscriber::AeronSubscriber(std::string channel,
                                 std::int32_t stream_id,
                                 Ring& ring,
                                 ThreadStats& stats,
                                 core::Source source,
                                 std::shared_ptr<aeron::Aeron> client,
                                 std::atomic<bool>& stop_flag) noexcept
    : AeronSubscriber(std::move(channel),
                      stream_id,
                      ring,
                      stats,
                      source,
                      make_aeron_client_view(std::move(client)),
                      stop_flag) {}

AeronSubscriber::AeronSubscriber(std::string channel,
                                 std::int32_t stream_id,
                                 Ring& ring,
                                 ThreadStats& stats,
                                 core::Source source,
                                 std::shared_ptr<AeronClientView> client,
                                 std::atomic<bool>& stop_flag) noexcept
    : channel_(std::move(channel))
    , stream_id_(stream_id)
    , ring_(ring)
    , stats_(stats)
    , source_(source)
    , client_(std::move(client))
    , stop_flag_(stop_flag) {}

void AeronSubscriber::run() noexcept {
    using namespace aeron;
    constexpr int fragment_limit = 10;

    std::shared_ptr<SubscriptionView> subscription;
    try {
        const auto registration_id = client_->add_subscription(channel_, stream_id_);

        while (!stop_flag_.load(std::memory_order_acquire) && !subscription) {
            subscription = client_->find_subscription(registration_id);
            if (!subscription) {
                std::this_thread::yield();
            }
        }
    } catch (...) {
        stats_.setup_failed.store(true, std::memory_order_release);
        return;
    }

    if (!subscription) {
        return;
    }

    auto handler = [&](const concurrent::AtomicBuffer& buffer,
                       aeron::util::index_t offset,
                       aeron::util::index_t length,
                       const concurrent::logbuffer::Header&) {
        if (length != static_cast<aeron::util::index_t>(sizeof(core::WireExecEvent))) {
            inc_relaxed(stats_.parse_failures);
            return;
        }

        auto* slot = ring_.try_reserve();
        if (!slot) {
            inc_relaxed(stats_.drops);
            return;
        }

        core::WireExecEvent wire;
        std::memcpy(&wire, buffer.buffer() + offset, sizeof(core::WireExecEvent));
        *slot = core::from_wire(wire, source_, ::util::rdtsc());
        ring_.commit();
        inc_relaxed(stats_.produced);
    };

    while (!stop_flag_.load(std::memory_order_acquire)) {
        PERF_START(aeron_poll);
        const int fragments = subscription->poll(handler, fragment_limit);
        if (fragments > 0) {
            PERF_STOP(aeron_poll, ::util::PerfCounterId::AeronPoll);
        }

        if (fragments == 0) {
            ::util::cpu_relax();
        }
    }
}

} // namespace ingest
