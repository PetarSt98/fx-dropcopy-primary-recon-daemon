#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>

namespace ingest {

// Single-producer/single-consumer ring buffer with cache-line separated indices.
// Producer uses relaxed load + release store; consumer uses relaxed load + acquire load and
// release store to publish consumption. Capacity must be a power of two.
template <typename T, std::size_t CapacityPowerOf2>
class alignas(64) SpscRing {
    static_assert((CapacityPowerOf2 & (CapacityPowerOf2 - 1)) == 0,
                  "Capacity must be power of two");

public:
    using value_type = T;

    SpscRing() : head_(0), tail_(0) {}

    bool try_push(const T& v) noexcept {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next_head = increment(head);
        if (next_head == tail_.load(std::memory_order_acquire)) {
            return false; // full
        }
        buffer_[head] = v;
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // empty
        }
        out = buffer_[tail];
        tail_.store(increment(tail), std::memory_order_release);
        return true;
    }

    std::size_t size_approx() const noexcept {
        const auto head = head_.load(std::memory_order_acquire);
        const auto tail = tail_.load(std::memory_order_acquire);
        return head >= tail ? head - tail : CapacityPowerOf2 - (tail - head);
    }

    static constexpr std::size_t capacity() noexcept { return CapacityPowerOf2; }

private:
    static constexpr std::size_t increment(std::size_t idx) noexcept {
        return (idx + 1) & (CapacityPowerOf2 - 1);
    }

    alignas(64) std::atomic<std::size_t> head_;
    alignas(64) std::atomic<std::size_t> tail_;
    T buffer_[CapacityPowerOf2];
};

} // namespace ingest
