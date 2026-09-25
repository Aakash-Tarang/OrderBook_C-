#pragma once
/*
 * test_framework.h – Minimal dependency-free test harness.
 *
 * The project deliberately avoids third-party test dependencies so the
 * whole repository builds with a bare `cmake && make` on any machine
 * (including a stock macOS + Apple Clang install).  The harness provides:
 *
 *   OB_TEST(name) { ... }   – register a test function
 *   OB_CHECK(expr)          – soft failure: records, continues the test
 *   OB_REQUIRE(expr)        – hard failure: records, aborts the test
 *   running the binary executes every test and returns non-zero on failure
 */

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace ob_test {

struct Failure {
    std::string file;
    int         line;
    std::string message;
};

struct TestCase {
    const char*              name;
    std::function<void()>    fn;
};

inline std::vector<TestCase>& tests()
{
    static std::vector<TestCase> t;
    return t;
}

inline std::vector<Failure>& failures()
{
    static std::vector<Failure> f;
    return f;
}

inline thread_local int g_current_failures = 0;

struct Registrar {
    Registrar(const char* name, std::function<void()> fn)
    {
        tests().push_back({name, std::move(fn)});
    }
};

inline int run_all()
{
    int failed_tests = 0;
    for (const auto& t : tests()) {
        std::fprintf(stderr, ">> %s\n", t.name);   // progress: survives buffering
        const int before = static_cast<int>(failures().size());
        g_current_failures = 0;
        t.fn();
        const int after = static_cast<int>(failures().size());
        if (after > before) {
            ++failed_tests;
            std::printf("[FAIL] %s\n", t.name);
            for (int i = before; i < after; ++i)
                std::printf("       %s:%d  %s\n",
                            failures()[i].file.c_str(),
                            failures()[i].line,
                            failures()[i].message.c_str());
        } else {
            std::printf("[ ok ] %s\n", t.name);
        }
    }
    std::printf("\n%zu tests, %d failed\n", tests().size(), failed_tests);
    return failed_tests == 0 ? 0 : 1;
}

} // namespace ob_test

#define OB_TEST(name)                                                        \
    static void ob_test_fn_##name();                                         \
    static ::ob_test::Registrar ob_test_reg_##name{#name, ob_test_fn_##name};\
    static void ob_test_fn_##name()

#define OB_CHECK_IMPL(expr, is_require)                                      \
    do {                                                                     \
        if (!(expr)) {                                                       \
            ::ob_test::failures().push_back(                                 \
                {__FILE__, __LINE__, "check failed: " #expr});               \
            if (is_require) return;                                          \
            ++::ob_test::g_current_failures;                                 \
        }                                                                    \
    } while (0)

#define OB_CHECK(expr)    OB_CHECK_IMPL(expr, false)
#define OB_REQUIRE(expr)  OB_CHECK_IMPL(expr, true)

// Record a custom failure message.
#define OB_FAIL(msg)                                                         \
    do {                                                                     \
        ::ob_test::failures().push_back(                                     \
            {__FILE__, __LINE__, ::std::string(msg)});                       \
        ++::ob_test::g_current_failures;                                     \
    } while (0)


// main() lives in tests/test_main.cpp so several test translation units
// can be linked into a single test binary.
