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
#include <string>
#include <type_traits>

// Renders an asserted value for the failure message. Assertions must stay usable
// on any comparable type (the demux suite compares std::string codec IDs), so
// this dispatches on the type instead of blanket-casting to long long.
template <typename T>
inline void test_print_value(const T& v) {
  if constexpr (std::is_same_v<T, std::string>) {
    printf("\"%s\"", v.c_str());
  } else if constexpr (std::is_same_v<T, const char*> ||
                       std::is_same_v<T, char*>) {
    printf("\"%s\"", v ? v : "(null)");
  } else if constexpr (std::is_same_v<T, bool>) {
    printf("%s", v ? "true" : "false");
  } else if constexpr (std::is_enum_v<T>) {
    printf("%lld", static_cast<long long>(v));
  } else if constexpr (std::is_integral_v<T>) {
    printf("%lld", static_cast<long long>(v));
  } else if constexpr (std::is_floating_point_v<T>) {
    printf("%g", static_cast<double>(v));
  } else if constexpr (std::is_pointer_v<T>) {
    printf("%p", static_cast<const void*>(v));
  } else {
    printf("<value>");
  }
}

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

#define TEST_FAIL_HEAD_(op, a, b) \
  printf("FAIL\n    " op "(%s, %s) [", a, b)

#define TEST_FAIL_TAIL_()                             \
  do {                                                \
    printf("] at %s:%d\n", __FILE__, __LINE__);       \
    g_current_test_failed = true;                     \
  } while (0)

#define ASSERT_CMP_(op, sym, a, b)                    \
  do {                                                \
    auto _a = (a);                                    \
    auto _b = (b);                                    \
    if (!(_a sym _b)) {                               \
      TEST_FAIL_HEAD_(op, #a, #b);                    \
      test_print_value(_a);                           \
      printf(" vs ");                                 \
      test_print_value(_b);                           \
      TEST_FAIL_TAIL_();                              \
      return;                                         \
    }                                                 \
  } while (0)

#define ASSERT_TRUE(expr)                                              \
  do {                                                                 \
    if (!(expr)) {                                                     \
      printf("FAIL\n    ASSERT_TRUE(%s) at %s:%d\n", #expr, __FILE__,  \
             __LINE__);                                                \
      g_current_test_failed = true;                                    \
      return;                                                          \
    }                                                                  \
  } while (0)

#define ASSERT_FALSE(expr) ASSERT_TRUE(!(expr))

#define ASSERT_EQ(a, b) ASSERT_CMP_("ASSERT_EQ", ==, a, b)
#define ASSERT_NE(a, b) ASSERT_CMP_("ASSERT_NE", !=, a, b)
#define ASSERT_GE(a, b) ASSERT_CMP_("ASSERT_GE", >=, a, b)
#define ASSERT_GT(a, b) ASSERT_CMP_("ASSERT_GT", >, a, b)
#define ASSERT_LE(a, b) ASSERT_CMP_("ASSERT_LE", <=, a, b)
#define ASSERT_LT(a, b) ASSERT_CMP_("ASSERT_LT", <, a, b)

#define TEST_MAIN(banner)                                             \
  int main() {                                                        \
    printf("\n=== " banner " ===\n");                                 \
    printf("\n--- Results: %d passed, %d failed ---\n\n",             \
           g_tests_passed, g_tests_failed);                           \
    return g_tests_failed > 0 ? 1 : 0;                                \
  }
