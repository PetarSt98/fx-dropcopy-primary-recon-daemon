#include "test_main.hpp"
#include "core/order_lifecycle.hpp"

namespace order_lifecycle_tests {

bool test_valid_transitions_apply() {
    core::OrdStatus status = core::OrdStatus::New;
    if (!core::apply_status_transition(status, core::OrdStatus::Working)) return false;
    if (status != core::OrdStatus::Working) return false;
    if (!core::apply_status_transition(status, core::OrdStatus::PartiallyFilled)) return false;
    if (status != core::OrdStatus::PartiallyFilled) return false;
    if (!core::apply_status_transition(status, core::OrdStatus::Filled)) return false;
    return status == core::OrdStatus::Filled;
}

bool test_invalid_transitions_rejected() {
    core::OrdStatus status = core::OrdStatus::Filled;
    if (core::apply_status_transition(status, core::OrdStatus::New)) return false;
    if (status != core::OrdStatus::Filled) return false;

    status = core::OrdStatus::Canceled;
    if (core::apply_status_transition(status, core::OrdStatus::Working)) return false;
    if (status != core::OrdStatus::Canceled) return false;

    status = core::OrdStatus::PartiallyFilled;
    if (core::apply_status_transition(status, core::OrdStatus::Working)) return false;
    return status == core::OrdStatus::PartiallyFilled;
}

bool test_unknown_accepts_first_status() {
    core::OrdStatus status = core::OrdStatus::Unknown;
    if (!core::is_valid_transition(status, core::OrdStatus::New)) return false;
    if (!core::apply_status_transition(status, core::OrdStatus::New)) return false;
    return status == core::OrdStatus::New;
}

void add_tests(std::vector<TestCase>& tests) {
    tests.push_back({"order_lifecycle_valid_transitions_apply", test_valid_transitions_apply});
    tests.push_back({"order_lifecycle_invalid_transitions_rejected", test_invalid_transitions_rejected});
    tests.push_back({"order_lifecycle_unknown_accepts_first_status", test_unknown_accepts_first_status});
}

} // namespace order_lifecycle_tests

