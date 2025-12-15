#include "ingest/aeron_subscriber.hpp"

#include <thread>

#include "util/rdtsc.hpp"

namespace ingest {

AeronSubscriber::AeronSubscriber(std::string channel,
                                 std::int32_t stream_id,
                                 Ring& ring,
                                 ThreadStats& stats,
                                 core::Source source,
                                 std::shared_ptr<aeron::Aeron> client,
                                 std::atomic<bool>& stop_flag) noexcept
    : channel_(std::move(channel))
    , stream_id_(stream_id)
    , ring_(ring)
    , stats_(stats)
    , source_(source)
    , client_(std::move(client))
    , stop_flag_(stop_flag) {}

void AeronSubscriber::run() {
    using namespace aeron;
    constexpr int fragment_limit = 10;

    std::shared_ptr<Subscription> subscription =
        client_->addSubscription(channel_, stream_id_);

    auto handler = [&](const concurrent::AtomicBuffer& buffer,
                       util::index_t offset,
                       util::index_t length,
                       const Header&) {
        if (length != static_cast<util::index_t>(sizeof(core::WireExecEvent))) {
            ++stats_.parse_failures;
            return;
        }

        const auto* wire = reinterpret_cast<const core::WireExecEvent*>(buffer.buffer() + offset);
        const core::ExecEvent evt = core::from_wire(*wire, source_, util::rdtsc());
        if (!ring_.try_push(evt)) {
            ++stats_.drops;
        } else {
            ++stats_.produced;
        }
    };

    int idle_count = 0;
    while (!stop_flag_.load(std::memory_order_acquire)) {
        const int fragments = subscription->poll(handler, fragment_limit);
        if (fragments == 0) {
            if (idle_count < 32) {
                ++idle_count;
            } else {
                idle_count = 0;
                std::this_thread::yield();
            }
        } else {
            idle_count = 0;
        }
    }
}

} // namespace ingest
