#include "test_main.hpp"

#include "core/divergence.hpp"

namespace divergence_tests {

bool test_phantom_order_detection() {
    core::OrderState state{};
    state.key = 1;
    state.seen_dropcopy = true;
    state.dropcopy_status = core::OrdStatus::New;
    state.last_dropcopy_ts = 100;

    core::Divergence div{};
    if (!core::classify_divergence(state, div)) return false;
    return div.type == core::DivergenceType::PhantomOrder && div.key == state.key;
}

bool test_missing_fill_detection() {
    core::OrderState state{};
    state.key = 2;
    state.seen_internal = true;
    state.seen_dropcopy = true;
    state.internal_status = core::OrdStatus::Working;
    state.internal_cum_qty = 10;
    state.internal_avg_px = 100;
    state.last_internal_ts = 200;
    state.dropcopy_status = core::OrdStatus::Filled;
    state.dropcopy_cum_qty = 20;
    state.dropcopy_avg_px = 105;
    state.last_dropcopy_ts = 150;

    core::Divergence div{};
    if (!core::classify_divergence(state, div)) return false;
    return div.type == core::DivergenceType::MissingFill &&
           div.internal_status == core::OrdStatus::Working &&
           div.dropcopy_status == core::OrdStatus::Filled;
}

bool test_state_mismatch_detection() {
    core::OrderState state{};
    state.key = 3;
    state.seen_internal = true;
    state.seen_dropcopy = true;
    state.internal_status = core::OrdStatus::PartiallyFilled;
    state.dropcopy_status = core::OrdStatus::Filled;
    state.internal_cum_qty = 15;
    state.dropcopy_cum_qty = 15;

    core::Divergence div{};
    if (!core::classify_divergence(state, div)) return false;
    return div.type == core::DivergenceType::StateMismatch &&
           div.internal_status == core::OrdStatus::PartiallyFilled &&
           div.dropcopy_status == core::OrdStatus::Filled;
}

bool test_quantity_mismatch_detection() {
    core::OrderState state{};
    state.key = 4;
    state.seen_internal = true;
    state.seen_dropcopy = true;
    state.internal_status = core::OrdStatus::Filled;
    state.dropcopy_status = core::OrdStatus::Filled;
    state.internal_cum_qty = 10;
    state.dropcopy_cum_qty = 20;
    state.internal_avg_px = 100;
    state.dropcopy_avg_px = 100;

    core::Divergence div{};
    if (!core::classify_divergence(state, div, 5)) return false;
    return div.type == core::DivergenceType::QuantityMismatch &&
           div.dropcopy_cum_qty == 20 &&
           div.internal_cum_qty == 10;
}

bool test_timing_anomaly_detection() {
    core::OrderState state{};
    state.key = 5;
    state.seen_internal = true;
    state.seen_dropcopy = true;
    state.internal_status = core::OrdStatus::PartiallyFilled;
    state.dropcopy_status = core::OrdStatus::PartiallyFilled;
    state.internal_cum_qty = 10;
    state.dropcopy_cum_qty = 10;
    state.last_dropcopy_ts = 100;
    state.last_internal_ts = 200;

    core::Divergence div{};
    if (!core::classify_divergence(state, div, 0, 0, 50)) return false;
    return div.type == core::DivergenceType::TimingAnomaly &&
           div.dropcopy_ts == state.last_dropcopy_ts &&
           div.internal_ts == state.last_internal_ts;
}

void add_tests(std::vector<TestCase>& tests) {
    tests.push_back({"divergence_phantom_order_detection", test_phantom_order_detection});
    tests.push_back({"divergence_missing_fill_detection", test_missing_fill_detection});
    tests.push_back({"divergence_state_mismatch_detection", test_state_mismatch_detection});
    tests.push_back({"divergence_quantity_mismatch_detection", test_quantity_mismatch_detection});
    tests.push_back({"divergence_timing_anomaly_detection", test_timing_anomaly_detection});
}

} // namespace divergence_tests

