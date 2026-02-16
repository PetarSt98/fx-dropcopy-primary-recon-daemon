#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>

#include "core/order_state.hpp"
#include "core/exec_event.hpp"
#include "util/arena.hpp"

namespace core {

// OrderStateStore is a single-writer, open-addressed hash table keyed by OrderKey.
// The reconciler thread is the only writer; future readers will be read-only.
//
// Deletion strategy: backward-shift (no tombstones).
//   When an entry is recycled, subsequent entries in the same probe cluster are
//   shifted backward to fill the gap. This keeps probe chains minimal and avoids
//   the tombstone accumulation that degrades linear probing over time.
//   The shift is O(cluster_length) worst case, but clusters are short at typical
//   load factors (< 50%).
//
// Memory: OrderState instances are allocated from the provided Arena (append-only,
//   bulk reset via reset_epoch()). On recycle, the arena memory is not freed but
//   is cached in a freelist for O(1) reuse by the next upsert.
class OrderStateStore {
public:
    // May throw std::invalid_argument on an unusable capacity_hint or
    // std::runtime_error if the bucket sizing overflows at construction time.
    OrderStateStore(util::Arena& arena, std::size_t capacity_hint);

    OrderStateStore(const OrderStateStore&) = delete;
    OrderStateStore& operator=(const OrderStateStore&) = delete;
    OrderStateStore(OrderStateStore&&) = delete;
    OrderStateStore& operator=(OrderStateStore&&) = delete;

    OrderState* upsert(const ExecEvent& ev) noexcept;
    OrderState* find(OrderKey key) noexcept;
    void recycle(OrderKey key) noexcept;
    void reset_epoch() noexcept;

    std::size_t bucket_count() const noexcept { return bucket_count_; }
    std::size_t size() const noexcept { return size_; }
    std::size_t overflow_count() const noexcept { return overflow_count_; }
    std::size_t recycled_count() const noexcept { return recycled_count_; }

private:
    static constexpr OrderKey empty_key_ = std::numeric_limits<OrderKey>::max();

    static std::size_t next_power_of_two(std::size_t v);

    std::size_t mask() const noexcept { return bucket_count_ - 1; }
    std::size_t hash(OrderKey key) const noexcept { return key; }

    // Allocate or reuse an OrderState slot.
    OrderState* alloc_state(OrderKey key) noexcept;

    // Return an OrderState slot to the freelist for reuse.
    void free_state(OrderState* st) noexcept;

    util::Arena& arena_;
    std::unique_ptr<OrderKey[]> keys_;
    std::unique_ptr<OrderState*[]> values_;
    std::size_t bucket_count_{0};
    std::size_t size_{0};
    std::size_t overflow_count_{0};
    std::size_t recycled_count_{0};

    // Intrusive freelist of recycled OrderState slots (arena memory reuse).
    // We store the next pointer in the first 8 bytes of the recycled OrderState.
    OrderState* free_head_{nullptr};
};

} // namespace core
