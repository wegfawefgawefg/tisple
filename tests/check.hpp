#pragma once

#include <cstdio>

namespace tisple_tests {

struct TestResults {
    int passed = 0;
    int failed = 0;
};

inline void Check(TestResults& results, bool ok, const char* what, const char* file, int line) {
    if (ok) {
        results.passed++;
        return;
    }
    results.failed++;
    std::fprintf(stderr, "%s:%d: check failed: %s\n", file, line, what);
}

} // namespace tisple_tests

#define CHECK(results, cond) tisple_tests::Check(results, (cond), #cond, __FILE__, __LINE__)
