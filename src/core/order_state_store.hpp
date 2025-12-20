#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "core/order_state.hpp"
#include "core/exec_event.hpp"
#include "util/arena.hpp"

namespace core {

// OrderStateStore is a single-writer, open-addressed hash table keyed by OrderKey.
// The reconciler thread is the only writer; future readers will be read-only.
// Buckets are allocated once in the constructor (heap), while OrderState
// instances are allocated from the provided Arena. The hot path (upsert/find)
// performs no allocations and is noexcept.
class OrderStateStore {
public:
    // May throw std::invalid_argument on an unusable capacity_hint or std::runtime_error
    // if the bucket sizing overflows at construction time.
    OrderStateStore(util::Arena& arena, std::size_t capacity_hint);

    OrderStateStore(const OrderStateStore&) = delete;
    OrderStateStore& operator=(const OrderStateStore&) = delete;
    OrderStateStore(OrderStateStore&&) = delete;
    OrderStateStore& operator=(OrderStateStore&&) = delete;

    OrderState* upsert(const ExecEvent& ev) noexcept;
    OrderState* find(OrderKey key) noexcept;
    void reset_epoch() noexcept;

    std::size_t bucket_count() const noexcept { return bucket_count_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t overflow_count() const noexcept { return overflow_count_; }

private:
    static constexpr OrderKey empty_key_ = 0; // make_order_key is assumed never to return 0.

    static std::size_t next_power_of_two(std::size_t v);

    std::size_t mask() const noexcept { return bucket_count_ - 1; }
    std::size_t hash(OrderKey key) const noexcept { return key; }

    util::Arena& arena_;
    std::unique_ptr<OrderKey[]> keys_;
    std::unique_ptr<OrderState*[]> values_;
    std::size_t bucket_count_{0};
    std::size_t size_{0};
    std::size_t overflow_count_{0};
    std::size_t max_probe_{0};
};

} // namespace core
