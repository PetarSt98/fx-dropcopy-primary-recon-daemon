#include <cstdint>

#include "test_main.hpp"
#include "util/arena.hpp"

namespace arena_tests {

bool test_basic_allocation() {
    util::Arena arena(64);
    void* p1 = arena.allocate(16, alignof(std::uint64_t));
    void* p2 = arena.allocate(16, alignof(std::uint32_t));
    return p1 != nullptr && p2 != nullptr && p1 != p2;
}

bool test_alignment() {
    util::Arena arena(64);
    constexpr std::size_t alignment = 32;
    void* p = arena.allocate(8, alignment);
    return p != nullptr && (reinterpret_cast<std::uintptr_t>(p) % alignment == 0);
}

bool test_exhaustion() {
    util::Arena arena(32);
    void* p1 = arena.allocate(16, alignof(std::uint64_t));
    void* p2 = arena.allocate(16, alignof(std::uint64_t));
    void* p3 = arena.allocate(8, alignof(std::uint64_t));
    return p1 != nullptr && p2 != nullptr && p3 == nullptr;
}

bool test_reset_reuse() {
    util::Arena arena(32);
    void* first = arena.allocate(24, alignof(std::uint64_t));
    void* exhausted = arena.allocate(16, alignof(std::uint64_t));
    arena.reset();
    void* after_reset = arena.allocate(24, alignof(std::uint64_t));
    return first != nullptr && exhausted == nullptr && after_reset == first;
}

void add_tests(std::vector<TestCase>& tests) {
    tests.push_back({"arena_basic_allocation", test_basic_allocation});
    tests.push_back({"arena_alignment", test_alignment});
    tests.push_back({"arena_exhaustion", test_exhaustion});
    tests.push_back({"arena_reset_reuse", test_reset_reuse});
}

} // namespace arena_tests
