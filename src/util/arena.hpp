#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

namespace util {

// Arena is a day/session-scoped bump allocator owned by a single thread
// (reconciler). It performs no internal locking, throws only from the
// constructor if the backing store allocation fails, and never allocates
// during the hot path beyond the initial backing store. Memory can be
// reclaimed in bulk via reset() outside of latency-critical paths.
class Arena {
public:
    static constexpr std::size_t default_capacity_bytes = 512ULL * 1024ULL * 1024ULL;

    explicit Arena(std::size_t capacity_bytes = default_capacity_bytes)
        : capacity_bytes_{capacity_bytes},
          buffer_{capacity_bytes ? std::make_unique<std::byte[]>(capacity_bytes) : nullptr} {}

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = default;
    Arena& operator=(Arena&&) = default;

    [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment) noexcept {
        if (alignment == 0) {
            return nullptr;
        }

        const std::size_t aligned_offset = align_up(offset_, alignment);
        if (aligned_offset > capacity_bytes_) {
            return nullptr;
        }

        if (capacity_bytes_ - aligned_offset < size) {
            return nullptr;
        }

        void* ptr = buffer_.get() + aligned_offset;
        offset_ = aligned_offset + size;
        return ptr;
    }

    void reset() noexcept { offset_ = 0; }

private:
    static constexpr std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
        const std::size_t remainder = value % alignment;
        return remainder == 0 ? value : value + (alignment - remainder);
    }

    std::size_t capacity_bytes_{0};
    std::unique_ptr<std::byte[]> buffer_{};
    std::size_t offset_{0};
};

} // namespace util
