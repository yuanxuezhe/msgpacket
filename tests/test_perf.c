/*
 * test_perf.c — MsgPacket 性能测试（MinUnit 框架）
 * 包含 create_destroy / encode_decode / clone 三个场景
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

#include "minunit.h"
#include "msg_api.h"
#include "msg_util.h"
#include "msg_packet.h"
#include "msg_byteorder.h"

/* ================================================================
 * 辅助函数（从 test_msgpacket.c 提取）
 * ================================================================ */

static const char *get_row_value_by_key(msg_packet_t *p, const char *key) {
    size_t col_count = msg_get_header_count(p);
    if (col_count == 0) return NULL;

    char headers[256];
    size_t hdr_buf_len = sizeof(headers);
    if (msg_get_headers(p, headers, &hdr_buf_len) != 0) return NULL;

    char *saveptr = NULL;
    char *token = strtok_r(headers, ",", &saveptr);
    int col_idx = -1;
    for (size_t i = 0; token != NULL; i++) {
        if (strcasecmp(token, key) == 0) {
            col_idx = (int)i;
            break;
        }
        token = strtok_r(NULL, ",", &saveptr);
    }
    if (col_idx < 0) return NULL;

    static char val_buf[256];
    const char *val = NULL;
    size_t val_len = 0;
    if (msg_get_value_str(p, key, &val, &val_len) != 0) return NULL;
    if (val_len >= sizeof(val_buf)) val_len = sizeof(val_buf) - 1;
    memcpy(val_buf, val, val_len);
    val_buf[val_len] = '\0';
    return val_buf;
}

/* ================================================================
 * 性能计时器
 * ================================================================ */

static uint64_t perf_get_time_ms(void) {
    clock_t c = clock();
    return (uint64_t)((double)c / CLOCKS_PER_SEC * 1000.0);
}

/* ================================================================
 * 场景1：create_destroy
 * ================================================================ */
MU_TEST(test_perf_create_destroy) {
    const int iterations = 10000;
    uint64_t start = perf_get_time_ms();

    for (int i = 0; i < iterations; i++) {
        msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
        mu_check(p != NULL);
        msg_destroy(p);
    }

    uint64_t elapsed = perf_get_time_ms() - start;
    printf("\n  [perf] create_destroy: %llu ms for %d iterations (%.2f us/op)",
           (unsigned long long)elapsed, iterations,
           (double)elapsed * 1000.0 / iterations);
    mu_check(elapsed < 10000);  /* 10s 超时保护 */
    return 0;
}

/* ================================================================
 * 场景2：encode_decode
 * ================================================================ */
MU_TEST(test_perf_encode_decode) {
    const int iterations = 5000;

    /* 构建一个典型数据包 */
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    msg_set_func(p, "query");
    msg_set_headers(p, 4, "Symbol,Price,Volume,Time");
    msg_add_row(p);
    msg_set_value_str(p, "Symbol", "BTCUSD");
    msg_set_value_str(p, "Price", "50000.50");
    msg_set_value_str(p, "Volume", "1000");
    msg_set_value_str(p, "Time", "20250505120000000");
    mu_assert_int_eq(0, msg_finalize(p));

    /* 预热 */
    void *buf = NULL;
    size_t buf_len = 0;
    mu_assert_int_eq(0, msg_encode(p, &buf, &buf_len));
    msg_packet_t *d = NULL;
    mu_assert_int_eq(0, msg_decode(buf, buf_len, &d));
    msg_destroy(d);
    msg_free_buffer(buf);

    /* 正式计时 */
    uint64_t start = perf_get_time_ms();

    for (int i = 0; i < iterations; i++) {
        buf = NULL;
        buf_len = 0;
        mu_assert_int_eq(0, msg_encode(p, &buf, &buf_len));
        d = NULL;
        mu_assert_int_eq(0, msg_decode(buf, buf_len, &d));
        msg_destroy(d);
        msg_free_buffer(buf);
    }

    uint64_t elapsed = perf_get_time_ms() - start;
    printf("\n  [perf] encode_decode: %llu ms for %d iterations (%.2f us/op)",
           (unsigned long long)elapsed, iterations,
           (double)elapsed * 1000.0 / iterations);
    mu_check(elapsed < 30000);  /* 30s 超时保护 */

    msg_destroy(p);
    return 0;
}

/* ================================================================
 * 场景3：clone
 * ================================================================ */
MU_TEST(test_perf_clone) {
    const int iterations = 5000;

    /* 构建并编码解码一个典型数据包 */
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    msg_set_func(p, "query");
    msg_set_headers(p, 4, "Symbol,Price,Volume,Time");
    msg_add_row(p);
    msg_set_value_str(p, "Symbol", "ETHUSD");
    msg_set_value_str(p, "Price", "3000.75");
    msg_set_value_str(p, "Volume", "500");
    msg_set_value_str(p, "Time", "20250505120000000");
    mu_assert_int_eq(0, msg_finalize(p));

    void *buf = NULL;
    size_t buf_len = 0;
    mu_assert_int_eq(0, msg_encode(p, &buf, &buf_len));
    msg_destroy(p);

    msg_packet_t *decoded = NULL;
    mu_assert_int_eq(0, msg_decode(buf, buf_len, &decoded));
    mu_check(decoded != NULL);

    /* 需要先 fetch_next 才能用 get_row_value_by_key */
    mu_check(msg_fetch_next(decoded));

    /* 验证解码内容正确 */
    mu_assert_str_eq("ETHUSD", get_row_value_by_key(decoded, "Symbol"));
    mu_assert_str_eq("3000.75", get_row_value_by_key(decoded, "Price"));

    /* 预热 */
    msg_packet_t *c = msg_clone(decoded);
    mu_check(c != NULL);
    msg_destroy(c);

    /* 正式计时 */
    uint64_t start = perf_get_time_ms();

    for (int i = 0; i < iterations; i++) {
        c = msg_clone(decoded);
        mu_check(c != NULL);
        msg_destroy(c);
    }

    uint64_t elapsed = perf_get_time_ms() - start;
    printf("\n  [perf] clone: %llu ms for %d iterations (%.2f us/op)",
           (unsigned long long)elapsed, iterations,
           (double)elapsed * 1000.0 / iterations);
    mu_check(elapsed < 30000);  /* 30s 超时保护 */

    msg_destroy(decoded);
    msg_free_buffer(buf);
    return 0;
}

/* ================================================================
 * 测试套件
 * ================================================================ */
MU_TEST_SUITE(perf_tests) {
    MU_RUN_TEST(test_perf_create_destroy);
    MU_RUN_TEST(test_perf_encode_decode);
    MU_RUN_TEST(test_perf_clone);
}

/* ================================================================
 * main
 * ================================================================ */
int main(void) {
    printf("=== MsgPacket Performance Tests (MinUnit) ===\n\n");
    MU_RUN_SUITE(perf_tests);
    MU_PRINT_REPORT();
    return mu_failed > 0 ? 1 : 0;
}
