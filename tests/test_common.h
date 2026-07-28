// Shared test framework for the sanitizer suites.
// Provides TEST registration, ASSERT macros, and a main() template.
//
// Tests run from static initialisers, so registration order is file order and
// TEST_MAIN only reports the tallies.
//
// A failed ASSERT marks the current test and returns. The registrar then counts
// it as exactly one failure. (The predecessor's harness printed "PASS" and
// incremented the pass counter even after an assert had already printed "FAIL",
// so a run with one failing test out of two reported "2 passed, 1 failed".)
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_tests_passed = 0;
static int g_tests_failed = 0;
static bool g_current_test_failed = false;

#define TEST(name)                             \
  static void test_##name();                   \
  struct TestReg_##name {                      \
    TestReg_##name() {                         \
      printf("  %-60s", #name);                \
      fflush(stdout);                          \
      g_current_test_failed = false;           \
      test_##name();                           \
      if (g_current_test_failed) {             \
        g_tests_failed++;                      \
      } else {                                 \
        printf("PASS\n");                      \
        g_tests_passed++;                      \
      }                                        \
      fflush(stdout);                          \
    }                                          \
  };                                           \
  static TestReg_##name g_reg_##name;          \
  static void test_##name()

#define TEST_FAIL_(fmt, ...)                    \
  do {                                          \
    printf("FAIL\n    " fmt "\n", __VA_ARGS__); \
    g_current_test_failed = true;               \
  } while (0)

#define ASSERT_TRUE(expr)                                                 \
  do {                                                                    \
    if (!(expr)) {                                                        \
      TEST_FAIL_("ASSERT_TRUE(%s) at %s:%d", #expr, __FILE__, __LINE__);  \
      return;                                                             \
    }                                                                     \
  } while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(a, b)                                                       \
  do {                                                                        \
    auto _a = (a);                                                            \
    auto _b = (b);                                                            \
    if (_a != _b) {                                                           \
      TEST_FAIL_("ASSERT_EQ(%s, %s) [%lld != %lld] at %s:%d", #a, #b,         \
                 (long long)_a, (long long)_b, __FILE__, __LINE__);           \
      return;                                                                 \
    }                                                                         \
  } while (0)

#define ASSERT_NE(a, b)                                                       \
  do {                                                                        \
    auto _a = (a);                                                            \
    auto _b = (b);                                                            \
    if (_a == _b) {                                                           \
      TEST_FAIL_("ASSERT_NE(%s, %s) [both %lld] at %s:%d", #a, #b,            \
                 (long long)_a, __FILE__, __LINE__);                          \
      return;                                                                 \
    }                                                                         \
  } while (0)

#define ASSERT_GE(a, b)                                                       \
  do {                                                                        \
    auto _a = (a);                                                            \
    auto _b = (b);                                                            \
    if (_a < _b) {                                                            \
      TEST_FAIL_("ASSERT_GE(%s, %s) [%lld < %lld] at %s:%d", #a, #b,          \
                 (long long)_a, (long long)_b, __FILE__, __LINE__);           \
      return;                                                                 \
    }                                                                         \
  } while (0)

#define ASSERT_GT(a, b)                                                       \
  do {                                                                        \
    auto _a = (a);                                                            \
    auto _b = (b);                                                            \
    if (_a <= _b) {                                                           \
      TEST_FAIL_("ASSERT_GT(%s, %s) [%lld <= %lld] at %s:%d", #a, #b,         \
                 (long long)_a, (long long)_b, __FILE__, __LINE__);           \
      return;                                                                 \
    }                                                                         \
  } while (0)

#define ASSERT_LE(a, b)                                                       \
  do {                                                                        \
    auto _a = (a);                                                            \
    auto _b = (b);                                                            \
    if (_a > _b) {                                                            \
      TEST_FAIL_("ASSERT_LE(%s, %s) [%lld > %lld] at %s:%d", #a, #b,          \
                 (long long)_a, (long long)_b, __FILE__, __LINE__);           \
      return;                                                                 \
    }                                                                         \
  } while (0)

#define ASSERT_LT(a, b)                                                       \
  do {                                                                        \
    auto _a = (a);                                                            \
    auto _b = (b);                                                            \
    if (_a >= _b) {                                                           \
      TEST_FAIL_("ASSERT_LT(%s, %s) [%lld >= %lld] at %s:%d", #a, #b,         \
                 (long long)_a, (long long)_b, __FILE__, __LINE__);           \
      return;                                                                 \
    }                                                                         \
  } while (0)

#define TEST_MAIN(banner)                                             \
  int main() {                                                        \
    printf("\n=== " banner " ===\n");                                 \
    printf("\n--- Results: %d passed, %d failed ---\n\n",             \
           g_tests_passed, g_tests_failed);                           \
    return g_tests_failed > 0 ? 1 : 0;                                \
  }
