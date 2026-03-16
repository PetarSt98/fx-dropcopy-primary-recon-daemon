#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include "util/perf_macros.hpp"
#include <new> 
#include <sys/mman.h>
#include <cstring> 

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
    : capacity_bytes_{capacity_bytes}
{
    if (capacity_bytes_ == 0) return;

    void* p = ::mmap(nullptr,
                     capacity_bytes_,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);
    if (p == MAP_FAILED) {
        throw std::bad_alloc{};
    }

    // Hint the kernel to back this region with 2 MB transparent huge pages.
    // Must be called before pre-faulting so the page-fault handler sees the hint.
    ::madvise(p, capacity_bytes_, MADV_HUGEPAGE);

    // Pre-fault every page NOW (cold path) so the hot path never triggers
    // a kernel trap. memset touches every byte, which is the most reliable
    // way to guarantee all huge pages are physically mapped.
    std::memset(p, 0, capacity_bytes_);

    buffer_ = static_cast<std::byte*>(p);
}

    Arena(const Arena&) = delete;
    Arena &operator=(const Arena &) = delete;
    
    Arena(Arena&& other) noexcept
        : capacity_bytes_{other.capacity_bytes_}
        , buffer_{other.buffer_}
        , offset_{other.offset_}
    {
        other.buffer_       = nullptr;
        other.capacity_bytes_ = 0;
        other.offset_       = 0;
    }

    Arena& operator=(Arena&& other) noexcept {
        if (this != &other) {
            if (buffer_) ::munmap(buffer_, capacity_bytes_);
            capacity_bytes_     = other.capacity_bytes_;
            buffer_             = other.buffer_;
            offset_             = other.offset_;
            other.buffer_       = nullptr;
            other.capacity_bytes_ = 0;
            other.offset_       = 0;
        }
        return *this;
    }

    ~Arena() noexcept {
        if (buffer_) {
            ::munmap(buffer_, capacity_bytes_);
        }
    }

    [[nodiscard]] void* allocate(std::size_t size, std::size_t alignment) noexcept {
        PERF_SCOPE(::util::PerfCounterId::ArenaAllocate);
        
        assert(alignment > 0 && (alignment & (alignment - 1)) == 0
               && "alignment must be a non-zero power of 2");
        if (alignment == 0 || !buffer_) {
            return nullptr;
        }

        const auto base = reinterpret_cast<std::uintptr_t>(buffer_);
        const std::uintptr_t current = base + offset_;
        const std::uintptr_t aligned_addr = align_up(current, alignment);
        const std::size_t aligned_offset = aligned_addr - base;

        if (aligned_offset > capacity_bytes_) {
            return nullptr;
        }

        if (capacity_bytes_ - aligned_offset < size) {
            return nullptr;
        }

        void* ptr = buffer_ + aligned_offset;
        offset_ = aligned_offset + size;
        return ptr;
    }

    void reset() noexcept { offset_ = 0; }

private:
    static constexpr std::uintptr_t align_up(std::uintptr_t value, std::uintptr_t alignment) noexcept {
        return (value + alignment - 1u) & ~(alignment - 1u);
    }

    std::size_t capacity_bytes_{0};
    std::byte* buffer_{nullptr};
    std::size_t offset_{0};
};

} // namespace util
