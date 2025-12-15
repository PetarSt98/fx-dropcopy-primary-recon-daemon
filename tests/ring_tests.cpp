#include "test_main.hpp"
#include "ingest/spsc_ring.hpp"
#include "core/exec_event.hpp"

using Ring = ingest::SpscRing<core::ExecEvent, 8>;

namespace ring_tests {

bool test_push_pop_order() {
    Ring ring;
    core::ExecEvent a{}; a.qty = 1;
    core::ExecEvent b{}; b.qty = 2;
    core::ExecEvent out{};
    bool ok = ring.try_push(a);
    ok = ok && ring.try_push(b);
    ok = ok && ring.try_pop(out) && out.qty == 1;
    ok = ok && ring.try_pop(out) && out.qty == 2;
    return ok;
}

bool test_full_empty_behavior() {
    Ring ring;
    core::ExecEvent evt{};
    bool ok = true;
    for (int i = 0; i < 7; ++i) {
        ok = ok && ring.try_push(evt);
    }
    // ring is full now; next push should fail
    ok = ok && !ring.try_push(evt);
    for (int i = 0; i < 7; ++i) {
        ok = ok && ring.try_pop(evt);
    }
    ok = ok && !ring.try_pop(evt);
    return ok;
}

void add_tests(std::vector<TestCase>& tests) {
    tests.push_back({"push_pop_order", test_push_pop_order});
    tests.push_back({"full_empty_behavior", test_full_empty_behavior});
}

} // namespace ring_tests
