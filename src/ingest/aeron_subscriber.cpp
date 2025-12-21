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

void AeronSubscriber::run() {
    using namespace aeron;
    constexpr int fragment_limit = 10;

    const auto registration_id = client_->add_subscription(channel_, stream_id_);

    std::shared_ptr<SubscriptionView> subscription;
    while (!stop_flag_.load(std::memory_order_acquire) && !subscription) {
        subscription = client_->find_subscription(registration_id);
        if (!subscription) {
            std::this_thread::yield();
        }
    }

    if (!subscription) {
        return;
    }

    auto handler = [&](const concurrent::AtomicBuffer& buffer,
                       aeron::util::index_t offset,
                       aeron::util::index_t length,
                       const concurrent::logbuffer::Header&) {
        if (capture_writer_) {
            const auto* ptr = reinterpret_cast<const std::byte*>(buffer.buffer() + offset);
            capture_writer_->try_submit(std::span<const std::byte>(ptr, static_cast<std::size_t>(length)));
        }
        if (length != static_cast<aeron::util::index_t>(sizeof(core::WireExecEvent))) {
            ++stats_.parse_failures;
            return;
        }

        const auto* wire = reinterpret_cast<const core::WireExecEvent*>(buffer.buffer() + offset);
        const core::ExecEvent evt = core::from_wire(*wire, source_, ::util::rdtsc());
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
