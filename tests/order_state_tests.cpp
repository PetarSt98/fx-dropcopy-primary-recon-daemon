#include "test_main.hpp"
#include "core/order_state.hpp"

namespace order_state_tests {

bool test_order_key_stability() {
    core::ExecEvent evt{};
    evt.set_clord_id("ABC123", 6);
    const core::OrderKey key1 = core::make_order_key(evt);
    const core::OrderKey key2 = core::make_order_key(evt);

    core::ExecEvent other{};
    other.set_clord_id("XYZ789", 6);
    const core::OrderKey key3 = core::make_order_key(other);

    return key1 == key2 && key1 != key3;
}

bool test_create_order_state_initialization() {
    util::Arena arena(1024);
    constexpr core::OrderKey expected_key = 42;
    core::OrderState* state = core::create_order_state(arena, expected_key);
    if (!state) {
        return false;
    }

    bool ok = state->key == expected_key;
    ok = ok && state->internal_cum_qty == 0 && state->internal_avg_px == 0;
    ok = ok && state->dropcopy_cum_qty == 0 && state->dropcopy_avg_px == 0;
    ok = ok && state->last_internal_exec_id_len == 0 && state->last_dropcopy_exec_id_len == 0;
    ok = ok && !state->seen_internal && !state->seen_dropcopy;
    ok = ok && !state->has_divergence && !state->has_gap;
    ok = ok && state->divergence_count == 0;
    ok = ok && state->last_internal_exec_id[0] == '\0' && state->last_dropcopy_exec_id[0] == '\0';
    return ok;
}

void add_tests(std::vector<TestCase>& tests) {
    tests.push_back({"order_key_stability", test_order_key_stability});
    tests.push_back({"create_order_state_initialization", test_create_order_state_initialization});
}

} // namespace order_state_tests
