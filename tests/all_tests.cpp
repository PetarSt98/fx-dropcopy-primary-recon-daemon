#include "test_main.hpp"

namespace ring_tests { void add_tests(std::vector<TestCase>& tests); }
namespace fix_parser_tests { void add_tests(std::vector<TestCase>& tests); }

int main() {
    std::vector<TestCase> tests;
    ring_tests::add_tests(tests);
    fix_parser_tests::add_tests(tests);
    return run_tests(tests);
}
