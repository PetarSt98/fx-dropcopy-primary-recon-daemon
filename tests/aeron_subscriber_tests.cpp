#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <concurrent/AtomicBuffer.h>
#include <concurrent/logbuffer/FrameDescriptor.h>
#include <concurrent/logbuffer/LogBufferDescriptor.h>
#include <concurrent/logbuffer/Header.h>

#include "core/exec_event.hpp"
#include "core/wire_exec_event.hpp"
#include "ingest/aeron_client_view.hpp"
#include "ingest/aeron_subscriber.hpp"
#include "persist/wire_log_format.hpp"

namespace aeron_subscriber_tests {
namespace {

struct ThreadJoiner {
    std::thread& t;
    ~ThreadJoiner() {
        if (t.joinable()) {
            t.join();
        }
    }
};

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
    wire.seq_num = 1;
    wire.session_id = 0;
    wire.price_micro = 1;
    wire.qty = 2;
    wire.cum_qty = 2;
    wire.exec_id_len = 3;
    wire.exec_id[0] = 'E';
    wire.exec_id[1] = 'X';
    wire.exec_id[2] = '1';
    return wire;
}

class AeronSubscriberTest : public ::testing::Test {
protected:
    static constexpr std::chrono::milliseconds kWaitInterval{10};
    static constexpr std::chrono::milliseconds kMaxWait{1000};
};

TEST_F(AeronSubscriberTest, SubscriberDeliversFragment) {
    auto ring = std::make_unique<ingest::Ring>();
    ingest::ThreadStats stats;
    std::atomic<bool> stop{false};

    std::vector<std::uint8_t> payload(persist::wire_exec_event_wire_size);
    const auto wire = make_wire();
    persist::serialize_wire_exec_event(wire, payload.data());

    auto stub_sub = std::make_shared<StubSubscription>(std::vector{StubSubscription::Fragment{std::move(payload)}});
    auto client = std::make_shared<StubAeronClient>(stub_sub, 1);

    ingest::AeronSubscriber subscriber("", 1, *ring, stats, core::Source::Primary, client, stop);

    std::thread t([&] { subscriber.run(); });
    ThreadJoiner joiner{t};

    const auto deadline = std::chrono::steady_clock::now() + kMaxWait;
    while (stats.produced == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(kWaitInterval);
    }

    stop.store(true, std::memory_order_release);

    ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "Timed out waiting for subscriber to produce";

    core::ExecEvent evt{};
    ASSERT_TRUE(ring->try_pop(evt));
    EXPECT_EQ(stats.produced, 1);
    EXPECT_EQ(std::string_view(evt.exec_id, evt.exec_id_len), "EX1");
}

TEST_F(AeronSubscriberTest, SubscriberCountsParseFailure) {
    auto ring = std::make_unique<ingest::Ring>();
    ingest::ThreadStats stats;
    std::atomic<bool> stop{false};

    std::vector<std::uint8_t> payload(4, 0xFF); // invalid length
    auto stub_sub = std::make_shared<StubSubscription>(std::vector{StubSubscription::Fragment{std::move(payload)}});
    auto client = std::make_shared<StubAeronClient>(stub_sub, 0);

    ingest::AeronSubscriber subscriber("", 1, *ring, stats, core::Source::Primary, client, stop);

    std::thread t([&] { subscriber.run(); });
    ThreadJoiner joiner{t};

    const auto deadline = std::chrono::steady_clock::now() + kMaxWait;
    while (stats.parse_failures == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(kWaitInterval);
    }

    stop.store(true, std::memory_order_release);

    ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "Timed out waiting for parse failure";
    EXPECT_EQ(stats.parse_failures, 1);
    EXPECT_EQ(ring->size_approx(), 0u);
}

} // namespace
} // namespace aeron_subscriber_tests
