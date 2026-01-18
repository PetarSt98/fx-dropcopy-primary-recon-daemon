#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <array>
#include <cassert>

namespace util {

// FixedCapacityVec is a stack-allocated vector with compile-time max capacity.
// It never allocates on the heap and gracefully handles overflow by returning false.
//
// Use case: Timer wheel buckets where we want bounded memory and no allocations
// on the hot path, but need dynamic size within the fixed capacity.
//
// Thread safety: None.  Single-writer only (matches reconciler architecture).
template <typename T, std::size_t MaxCapacity>
class FixedCapacityVec {
    static_assert(MaxCapacity > 0, "MaxCapacity must be > 0");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable for performance");

public:
    using value_type = T;
    using size_type = std::size_t;
    using iterator = T*;
    using const_iterator = const T*;

    constexpr FixedCapacityVec() noexcept = default;

    // Attempt to add an element. Returns true on success, false if full.
    // Does NOT throw or crash on overflow.
    [[nodiscard]] bool try_push_back(const T& value) noexcept {
        if (size_ >= MaxCapacity) {
            return false;
        }
        data_[size_++] = value;
        return true;
    }

    // Attempt to construct an element in place. Returns true on success, false if full.
    // Note: For trivially copyable T, this constructs a temporary and assigns.
    // Safe to be noexcept because T is trivially copyable (required by static_assert).
    template <typename... Args>
    [[nodiscard]] bool try_emplace_back(Args&&... args) noexcept {
        if (size_ >= MaxCapacity) {
            return false;
        }
        data_[size_++] = T{std::forward<Args>(args)...};
        return true;
    }

    // Remove last element.  Asserts non-empty in debug builds.
    void pop_back() noexcept {
        assert(size_ > 0 && "pop_back on empty FixedCapacityVec");
        --size_;
    }

    // Clear all elements (O(1) - just resets size counter)
    void clear() noexcept {
        size_ = 0;
    }

    // Element access (no bounds checking - caller's responsibility)
    [[nodiscard]] constexpr T& operator[](size_type idx) noexcept { return data_[idx]; }
    [[nodiscard]] constexpr const T& operator[](size_type idx) const noexcept { return data_[idx]; }

    [[nodiscard]] constexpr T& front() noexcept { return data_[0]; }
    [[nodiscard]] constexpr const T& front() const noexcept { return data_[0]; }

    [[nodiscard]] constexpr T& back() noexcept { return data_[size_ - 1]; }
    [[nodiscard]] constexpr const T& back() const noexcept { return data_[size_ - 1]; }

    [[nodiscard]] constexpr T* data() noexcept { return data_.data(); }
    [[nodiscard]] constexpr const T* data() const noexcept { return data_.data(); }

    // Iterators
    [[nodiscard]] constexpr iterator begin() noexcept { return data_.data(); }
    [[nodiscard]] constexpr iterator end() noexcept { return data_.data() + size_; }
    [[nodiscard]] constexpr const_iterator begin() const noexcept { return data_.data(); }
    [[nodiscard]] constexpr const_iterator end() const noexcept { return data_.data() + size_; }
    [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return begin(); }
    [[nodiscard]] constexpr const_iterator cend() const noexcept { return end(); }

    // Size queries
    [[nodiscard]] constexpr size_type size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr bool full() const noexcept { return size_ >= MaxCapacity; }
    [[nodiscard]] static constexpr size_type max_size() noexcept { return MaxCapacity; }
    [[nodiscard]] static constexpr size_type capacity() noexcept { return MaxCapacity; }

    // Swap-and-pop removal (O(1) but doesn't preserve order)
    // Useful for removing expired timers without shifting.
    // Asserts valid index in debug builds.
    void swap_erase(size_type idx) noexcept {
        assert(idx < size_ && "swap_erase index out of bounds");
        if (idx < size_ - 1) {
            data_[idx] = data_[size_ - 1];
        }
        --size_;
    }

private:
    std::array<T, MaxCapacity> data_{};
    size_type size_{0};
};

} // namespace util
