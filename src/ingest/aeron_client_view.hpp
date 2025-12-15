#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <aeron/Aeron.h>
#include <aeron/AtomicBuffer.h>
#include <aeron/Subscription.h>
#include <aeron/concurrent/logbuffer/Header.h>

namespace ingest {

using FragmentHandler = std::function<void(const aeron::concurrent::AtomicBuffer&,
                                           aeron::util::index_t,
                                           aeron::util::index_t,
                                           const aeron::Header&)>;

class SubscriptionView {
public:
    virtual ~SubscriptionView() = default;
    virtual int poll(const FragmentHandler& handler, int fragment_limit) = 0;
};

class AeronClientView {
public:
    virtual ~AeronClientView() = default;
    virtual std::int64_t add_subscription(const std::string& channel, std::int32_t stream_id) = 0;
    virtual std::shared_ptr<SubscriptionView> find_subscription(std::int64_t registration_id) = 0;
};

class RealSubscriptionView final : public SubscriptionView {
public:
    explicit RealSubscriptionView(std::shared_ptr<aeron::Subscription> sub) : sub_(std::move(sub)) {}

    int poll(const FragmentHandler& handler, int fragment_limit) override {
        return sub_->poll(handler, fragment_limit);
    }

private:
    std::shared_ptr<aeron::Subscription> sub_;
};

class RealAeronClientView final : public AeronClientView {
public:
    explicit RealAeronClientView(std::shared_ptr<aeron::Aeron> client) : client_(std::move(client)) {}

    std::int64_t add_subscription(const std::string& channel, std::int32_t stream_id) override {
        return client_->addSubscription(channel, stream_id);
    }

    std::shared_ptr<SubscriptionView> find_subscription(std::int64_t registration_id) override {
        auto subscription = client_->findSubscription(registration_id);
        if (!subscription) {
            return nullptr;
        }
        return std::make_shared<RealSubscriptionView>(std::move(subscription));
    }

private:
    std::shared_ptr<aeron::Aeron> client_;
};

inline std::shared_ptr<AeronClientView> make_aeron_client_view(std::shared_ptr<aeron::Aeron> client) {
    return std::make_shared<RealAeronClientView>(std::move(client));
}

} // namespace ingest
