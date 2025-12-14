#pragma once

#include <cstddef>
#include <cstdint>
#include <new>

namespace util {
// Placeholder arena allocator stub for future use.
class Arena {
public:
    Arena() = default;
    void* allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t)) {
        (void)size; (void)alignment;
        return nullptr; // stub
    }
    void reset() {}
};
} // namespace util
