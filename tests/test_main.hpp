#pragma once

#include <iostream>
#include <string>
#include <vector>

struct TestCase {
    std::string name;
    bool (*fn)();
};

inline int run_tests(const std::vector<TestCase>& tests) {
    int failures = 0;
    for (const auto& t : tests) {
        const bool ok = t.fn();
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << t.name << "\n";
        if (!ok) ++failures;
    }
    std::cout << "Summary: " << (tests.size() - failures) << "/" << tests.size() << " passed\n";
    return failures;
}
