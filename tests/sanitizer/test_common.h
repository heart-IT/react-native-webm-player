// Shared test framework for sanitizer tests.
// Provides TEST registration, ASSERT macros, and main() template.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name) \
    static void test_##name(); \
    struct TestReg_##name { \
        TestReg_##name() { \
            printf("  %-60s", #name); \
            fflush(stdout); \
            test_##name(); \
            printf("PASS\n"); \
            g_tests_passed++; \
        } \
    }; \
    static TestReg_##name g_reg_##name; \
    static void test_##name()

#define ASSERT_TRUE(expr) do { \
    if (!(expr)) { \
        printf("FAIL\n    ASSERT_TRUE(%s) at %s:%d\n", #expr, __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { \
        printf("FAIL\n    ASSERT_EQ(%s, %s) at %s:%d\n", \
               #a, #b, __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_GE(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a < _b) { \
        printf("FAIL\n    ASSERT_GE(%s, %s) [%lld < %lld] at %s:%d\n", \
               #a, #b, (long long)_a, (long long)_b, __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_GT(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a <= _b) { \
        printf("FAIL\n    ASSERT_GT(%s, %s) [%lld <= %lld] at %s:%d\n", \
               #a, #b, (long long)_a, (long long)_b, __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_LE(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a > _b) { \
        printf("FAIL\n    ASSERT_LE(%s, %s) [%lld > %lld] at %s:%d\n", \
               #a, #b, (long long)_a, (long long)_b, __FILE__, __LINE__); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_MAIN(banner) \
    int main() { \
        printf("\n=== " banner " ===\n\n"); \
        printf("\n--- Results: %d passed, %d failed ---\n\n", \
               g_tests_passed, g_tests_failed); \
        return g_tests_failed > 0 ? 1 : 0; \
    }
