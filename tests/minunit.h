/*
 * minunit.h — 极简跨平台单元测试框架（header-only）
 *
 * 用法：
 *   #include "minunit.h"
 *   MU_TEST(test_name) {
 *       mu_check(condition);
 *       mu_assert_int_eq(expected, actual);
 *       mu_assert_str_eq(expected, actual);
 *       return NULL;
 *   }
 *   MU_TEST_SUITE(suite_name) {
 *       MU_RUN_TEST(test_name);
 *   }
 *   int main() {
 *       MU_RUN_SUITE(suite_name);
 *       MU_PRINT_REPORT();
 *       return mu_failed > 0 ? 1 : 0;
 *   }
 */
#ifndef MINUNIT_H
#define MINUNIT_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 全局计数器 */
static int mu_tests_run = 0;
static int mu_failed = 0;

/* 测试宏：条件为真则通过，否则打印并返回失败消息 */
#define mu_check(expr) do { \
    mu_tests_run++; \
    if (!(expr)) { \
        printf("%s:%d: FAILED: %s\n", __FILE__, __LINE__, #expr); \
        mu_failed++; \
        return #expr; \
    } \
} while (0)

/* 断言宏：失败时返回描述字符串，成功时继续 */
#define mu_assert_int_eq(expected, actual) do { \
    mu_tests_run++; \
    if ((expected) != (actual)) { \
        printf("%s:%d: FAILED: %d != %d\n", __FILE__, __LINE__, (int)(expected), (int)(actual)); \
        mu_failed++; \
        return "int mismatch"; \
    } \
} while (0)

#define mu_assert_str_eq(expected, actual) do { \
    mu_tests_run++; \
    if (strcmp((expected), (actual)) != 0) { \
        printf("%s:%d: FAILED: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (expected), (actual)); \
        mu_failed++; \
        return "string mismatch"; \
    } \
} while (0)

#define mu_assert_mem_eq(expected, actual, len) do { \
    mu_tests_run++; \
    if (memcmp((expected), (actual), (len)) != 0) { \
        printf("%s:%d: FAILED: memory mismatch\n", __FILE__, __LINE__); \
        mu_failed++; \
        return "memory mismatch"; \
    } \
} while (0)

#define mu_assert_double_eq(expected, actual, epsilon) do { \
    mu_tests_run++; \
    double _e = (expected); double _a = (actual); \
    if (_a < _e - (epsilon) || _a > _e + (epsilon)) { \
        printf("%s:%d: FAILED: %f != %f (epsilon=%f)\n", __FILE__, __LINE__, _e, _a, (double)(epsilon)); \
        mu_failed++; \
        return "double mismatch"; \
    } \
} while (0)

/* 测试函数定义：返回 NULL 表示通过，返回错误消息字符串表示失败 */
#define MU_TEST(name) const char * name(void)

/* 运行单个测试 */
#define MU_RUN_TEST(test) do { \
    const char *msg = test(); \
    mu_tests_run++; \
    if (msg) { \
        printf("%s:%d: FAILED: %s\n", __FILE__, __LINE__, msg); \
        mu_failed++; \
    } \
} while (0)

/* 测试套件定义 */
#define MU_TEST_SUITE(name) void name(void)

/* 运行测试套件 */
#define MU_RUN_SUITE(suite) do { \
    printf("=== %s ===\n", #suite); \
    suite(); \
} while (0)

/* 打印最终报告 */
#define MU_PRINT_REPORT() do { \
    printf("\n=== RESULT: %d/%d passed, %d failed ===\n", \
           (int)(mu_tests_run - mu_failed), (int)mu_tests_run, (int)mu_failed); \
} while (0)

#endif /* MINUNIT_H */
