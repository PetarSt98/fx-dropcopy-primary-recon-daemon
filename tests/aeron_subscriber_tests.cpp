#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <concurrent/AtomicBuffer.h>
#include <concurrent/logbuffer/FrameDescriptor.h>
#include <concurrent/logbuffer/LogBufferDescriptor.h>
#include <concurrent/logbuffer/Header.h>

#include "core/exec_event.hpp"
#include "core/wire_exec_event.hpp"
#include "ingest/aeron_client_view.hpp"
#include "ingest/aeron_subscriber.hpp"
#include "test_main.hpp"

namespace aeron_subscriber_tests {
namespace {

struct StubSubscription final : public ingest::SubscriptionView {
    struct Fragment {
        std::vector<std::uint8_t> payload;
    };

    explicit StubSubscription(std::vector<Fragment> fragments) : fragments_(std::move(fragments)) {}

    int poll(const ingest::FragmentHandler& handler, int fragment_limit) override {
        int handled = 0;
        while (handled < fragment_limit && !fragments_.empty()) {
            auto frag = std::move(fragments_.front());
            fragments_.erase(fragments_.begin());
            aeron::concurrent::AtomicBuffer buffer(frag.payload.data(), frag.payload.size());
            aeron::concurrent::logbuffer::Header header(
                /*initialTermId=*/0,
                /*capacity=*/static_cast<aeron::util::index_t>(frag.payload.size()),
                /*context=*/nullptr);
            handler(buffer, 0, static_cast<aeron::util::index_t>(frag.payload.size()), header);
            ++handled;
        }
        return handled;
    }

    std::vector<Fragment> fragments_;
};

struct StubAeronClient final : public ingest::AeronClientView {
    explicit StubAeronClient(std::shared_ptr<StubSubscription> sub, int misses_before_ready = 0)
        : subscription_(std::move(sub)), misses_before_ready_(misses_before_ready) {}

    std::int64_t add_subscription(const std::string&, std::int32_t) override { return 42; }

    std::shared_ptr<ingest::SubscriptionView> find_subscription(std::int64_t) override {
        if (misses_before_ready_-- > 0) {
            return nullptr;
        }
        return subscription_;
    }

    std::shared_ptr<StubSubscription> subscription_;
    int misses_before_ready_{0};
};

core::WireExecEvent make_wire() {
    core::WireExecEvent wire{};
    wire.exec_type = 2;
    wire.ord_status = 2;
    wire.price_micro = 1;
    wire.qty = 2;
    wire.cum_qty = 2;
    wire.exec_id_len = 3;
    wire.exec_id[0] = 'E';
    wire.exec_id[1] = 'X';
    wire.exec_id[2] = '1';
    return wire;
}

bool test_subscriber_delivers_fragment() {
    auto ring = std::make_unique<ingest::Ring>();
    ingest::ThreadStats stats;
    std::atomic<bool> stop{false};

    std::vector<std::uint8_t> payload(sizeof(core::WireExecEvent));
    const auto wire = make_wire();
    std::memcpy(payload.data(), &wire, sizeof(wire));

    auto stub_sub = std::make_shared<StubSubscription>(std::vector{StubSubscription::Fragment{std::move(payload)}});
    auto client = std::make_shared<StubAeronClient>(stub_sub, 1);

    ingest::AeronSubscriber subscriber("", 1, *ring, stats, core::Source::Primary, client, stop);

    std::thread t([&] { subscriber.run(); });
    while (stats.produced == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true, std::memory_order_release);
    t.join();

    core::ExecEvent evt{};
    return ring->try_pop(evt) && stats.produced == 1 && evt.exec_id[0] == 'E';
}

bool test_subscriber_counts_parse_failure() {
    auto ring = std::make_unique<ingest::Ring>();
    ingest::ThreadStats stats;
    std::atomic<bool> stop{false};

    std::vector<std::uint8_t> payload(4, 0xFF); // invalid length
    auto stub_sub = std::make_shared<StubSubscription>(std::vector{StubSubscription::Fragment{std::move(payload)}});
    auto client = std::make_shared<StubAeronClient>(stub_sub, 0);

    ingest::AeronSubscriber subscriber("", 1, *ring, stats, core::Source::Primary, client, stop);

    std::thread t([&] { subscriber.run(); });
    while (stats.parse_failures == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true, std::memory_order_release);
    t.join();

    return stats.parse_failures == 1 && ring->size_approx() == 0;
}

} // namespace

void add_tests(std::vector<TestCase>& tests) {
    tests.push_back({"subscriber_delivers_fragment", test_subscriber_delivers_fragment});
    tests.push_back({"subscriber_counts_parse_failure", test_subscriber_counts_parse_failure});
}

} // namespace aeron_subscriber_tests
