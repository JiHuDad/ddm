// test_framework.h — zero-dependency mini test runner.
//
// Standard library only; no GoogleTest/Catch2. Matches the project's
// "pure C++, no external deps" philosophy (see SPEC.md).
//
// Usage:
//   #include "test_framework.h"
//   TEST(my_case) {
//       CHECK(1 + 1 == 2);
//       CHECK_NEAR(0.1 + 0.2, 0.3, 1e-9);
//   }
//   int main() { return RUN_ALL(); }   // exit code = number of failed tests
#ifndef DRIFTMON_TEST_FRAMEWORK_H
#define DRIFTMON_TEST_FRAMEWORK_H

#include <cmath>
#include <cstdio>
#include <vector>

namespace dmtest {

using TestFn = void (*)(int& fail_count);

struct TestCase {
    const char* name;
    TestFn fn;
};

// Single global registry. Header-only, so guard against multiple definitions
// by keeping it in an inline-accessor with function-local static storage.
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, TestFn fn) { registry().push_back({name, fn}); }
};

inline int run_all() {
    int total_failures = 0;
    int failed_tests = 0;
    for (const auto& t : registry()) {
        int local = 0;
        t.fn(local);
        if (local == 0) {
            std::printf("[ PASS ] %s\n", t.name);
        } else {
            std::printf("[ FAIL ] %s (%d check(s) failed)\n", t.name, local);
            ++failed_tests;
            total_failures += local;
        }
    }
    std::printf("----\n%zu test(s), %d failed (%d check failure(s))\n",
                registry().size(), failed_tests, total_failures);
    return failed_tests;  // exit code
}

}  // namespace dmtest

// Define a test. The body receives an implicit `fail_count` reference used by
// the CHECK macros below.
#define TEST(name)                                                       \
    static void test_##name(int& fail_count);                            \
    static ::dmtest::Registrar registrar_##name(#name, &test_##name);    \
    static void test_##name(int& fail_count)

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("    CHECK failed: %s (%s:%d)\n", #cond,         \
                        __FILE__, __LINE__);                             \
            ++fail_count;                                                \
        }                                                                \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                            \
    do {                                                                 \
        double _da = (a), _db = (b), _de = (eps);                        \
        if (std::fabs(_da - _db) > _de) {                                \
            std::printf("    CHECK_NEAR failed: |%g - %g| > %g (%s:%d)\n",\
                        _da, _db, _de, __FILE__, __LINE__);              \
            ++fail_count;                                                \
        }                                                                \
    } while (0)

#define RUN_ALL() ::dmtest::run_all()

#endif  // DRIFTMON_TEST_FRAMEWORK_H
