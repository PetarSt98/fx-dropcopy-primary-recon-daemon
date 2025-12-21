#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <cstring>
#include <cstddef>

namespace persist {

struct CaptureDescriptor {
    std::uint32_t length{0};
    std::uint32_t slot_index{0};
};

template <std::size_t CapacityPow2, std::size_t SlotSize>
class CaptureRing {
    static_assert((CapacityPow2 & (CapacityPow2 - 1)) == 0, "Capacity must be power of two");

public:
    CaptureRing() = default;

    CaptureRing(const CaptureRing&) = delete;
    CaptureRing& operator=(const CaptureRing&) = delete;

    std::size_t capacity() const noexcept { return CapacityPow2; }
    static constexpr std::size_t capacity_static() noexcept { return CapacityPow2; }
    std::size_t slot_size() const noexcept { return SlotSize; }

    bool try_push(const std::byte* payload, std::size_t len) noexcept {
        if (producer_stop_.load(std::memory_order_relaxed)) {
            return false;
        }
        if (len > SlotSize) {
            return false;
        }
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);
        const auto next_tail = increment(tail);
        if (next_tail == head) {
            return false; // full
        }
        const std::size_t slot = tail & mask();
        std::byte* dest = slots_ + slot * SlotSize;
        std::memcpy(dest, payload, len);
        descriptors_[tail] = CaptureDescriptor{static_cast<std::uint32_t>(len), static_cast<std::uint32_t>(slot)};
        tail_.store(next_tail, std::memory_order_release);
        return true;
    }

    bool try_pop(CaptureDescriptor& desc) noexcept {
        const auto head_val = head_.load(std::memory_order_relaxed);
        if (head_val == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        desc = descriptors_[head_val];
        head_.store(increment(head_val), std::memory_order_release);
        return true;
    }

    std::byte* slot_ptr(std::uint32_t slot_index) noexcept {
        return slots_ + static_cast<std::size_t>(slot_index) * SlotSize;
    }

    void stop_producer() noexcept { producer_stop_.store(true, std::memory_order_release); }

    bool producer_stopped() const noexcept { return producer_stop_.load(std::memory_order_acquire); }

    std::size_t size_approx() const noexcept {
        const auto h = head_.load(std::memory_order_acquire);
        const auto t = tail_.load(std::memory_order_acquire);
        return t >= h ? t - h : CapacityPow2 - (h - t);
    }

private:
    static constexpr std::size_t mask() noexcept { return CapacityPow2 - 1; }
    static constexpr std::size_t increment(std::size_t v) noexcept { return (v + 1) & mask(); }

    alignas(64) std::atomic<std::size_t> head_{0};
    char pad1_[64 - sizeof(head_)]{};
    alignas(64) std::atomic<std::size_t> tail_{0};
    char pad2_[64 - sizeof(tail_)]{};
    alignas(64) CaptureDescriptor descriptors_[CapacityPow2]{};
    alignas(64) std::byte slots_[CapacityPow2 * SlotSize]{};
    std::atomic<bool> producer_stop_{false};
};

} // namespace persist
