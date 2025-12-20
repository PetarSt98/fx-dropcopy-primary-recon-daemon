#include "test_main.hpp"

namespace ring_tests { void add_tests(std::vector<TestCase>& tests); }
namespace fix_parser_tests { void add_tests(std::vector<TestCase>& tests); }
namespace wire_exec_tests { void add_tests(std::vector<TestCase>& tests); }
namespace aeron_subscriber_tests { void add_tests(std::vector<TestCase>& tests); }
namespace arena_tests { void add_tests(std::vector<TestCase>& tests); }
namespace order_state_tests { void add_tests(std::vector<TestCase>& tests); }
namespace order_lifecycle_tests { void add_tests(std::vector<TestCase>& tests); }
namespace divergence_tests { void add_tests(std::vector<TestCase>& tests); }
namespace order_state_store_tests { void add_tests(std::vector<TestCase>& tests); }
namespace reconciler_logic_tests { void add_tests(std::vector<TestCase>& tests); }
namespace sequence_tracker_tests { void add_tests(std::vector<TestCase>& tests); }
namespace reconciler_sequence_tests { void add_tests(std::vector<TestCase>& tests); }

int main() {
    std::vector<TestCase> tests;
    ring_tests::add_tests(tests);
    fix_parser_tests::add_tests(tests);
    wire_exec_tests::add_tests(tests);
    aeron_subscriber_tests::add_tests(tests);
    arena_tests::add_tests(tests);
    order_state_tests::add_tests(tests);
    order_lifecycle_tests::add_tests(tests);
    divergence_tests::add_tests(tests);
    order_state_store_tests::add_tests(tests);
    reconciler_logic_tests::add_tests(tests);
    sequence_tracker_tests::add_tests(tests);
    reconciler_sequence_tests::add_tests(tests);
    return run_tests(tests);
}
