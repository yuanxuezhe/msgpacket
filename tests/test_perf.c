/*
 * test_perf.c — MsgPacket 性能测试（MinUnit 框架）
 * 包含 create_destroy / encode_decode / clone 三个场景
 * 新增：QPS、延迟百分位(p50/p90/p99/p999)、吞吐率(MB/s)
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
 * 辅助函数
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
 * 性能计时器（微秒精度）
 * ================================================================ */

static uint64_t perf_get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

/* qsort 比较函数（升序） */
static int cmp_uint64(const void *a, const void *b) {
    uint64_t ua = *(const uint64_t *)a;
    uint64_t ub = *(const uint64_t *)b;
    return (ua > ub) - (ua < ub);
}

/* 打印百分位（假设 arr 已有序） */
static double percentile(uint64_t *arr, size_t n, double p) {
    if (n == 0) return 0;
    double idx = (n - 1) * p / 100.0;
    size_t lo = (size_t)idx;
    size_t hi = lo + 1;
    if (hi >= n) return (double)arr[n - 1];
    double frac = idx - lo;
    return arr[lo] * (1.0 - frac) + arr[hi] * frac;
}

/* ================================================================
 * 场景1：create_destroy
 * ================================================================ */
MU_TEST(test_perf_create_destroy) {
    const int iterations = 10000;

    /* 预热 */
    for (int i = 0; i < 100; i++) {
        msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
        msg_destroy(p);
    }

    /* 记录每次耗时 */
    uint64_t *latencies = (uint64_t *)malloc(sizeof(uint64_t) * iterations);
    mu_check(latencies != NULL);

    uint64_t start = perf_get_time_us();
    for (int i = 0; i < iterations; i++) {
        uint64_t t0 = perf_get_time_us();
        msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
        uint64_t t1 = perf_get_time_us();
        latencies[i] = t1 - t0;
        msg_destroy(p);
    }
    uint64_t elapsed = perf_get_time_us() - start;

    /* 排序 */
    qsort(latencies, iterations, sizeof(uint64_t), cmp_uint64);

    double qps = iterations * 1000000.0 / elapsed;
    double p50 = percentile(latencies, iterations, 50.0);
    double p90 = percentile(latencies, iterations, 90.0);
    double p99 = percentile(latencies, iterations, 99.0);
    double p999 = percentile(latencies, iterations, 99.9);

    printf("\n  [perf] create_destroy: %d iterations in %.3f ms\n", iterations, elapsed / 1000.0);
    printf("  [perf]   QPS: %.0f ops/s\n", qps);
    printf("  [perf]   latencies: p50=%.1f us  p90=%.1f us  p99=%.1f us  p999=%.1f us\n",
           p50, p90, p99, p999);

    free(latencies);
    mu_check(elapsed < 10000000ULL);  /* 10s 超时保护 */
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
    uint64_t *latencies_enc = (uint64_t *)malloc(sizeof(uint64_t) * iterations);
    uint64_t *latencies_dec = (uint64_t *)malloc(sizeof(uint64_t) * iterations);
    uint64_t *latencies_total = (uint64_t *)malloc(sizeof(uint64_t) * iterations);
    size_t *sizes = (size_t *)malloc(sizeof(size_t) * iterations);
    mu_check(latencies_enc && latencies_dec && latencies_total && sizes);

    uint64_t start = perf_get_time_us();
    for (int i = 0; i < iterations; i++) {
        /* encode */
        uint64_t t0 = perf_get_time_us();
        buf = NULL; buf_len = 0;
        mu_assert_int_eq(0, msg_encode(p, &buf, &buf_len));
        uint64_t t1 = perf_get_time_us();
        latencies_enc[i] = t1 - t0;
        sizes[i] = buf_len;

        /* decode */
        d = NULL;
        uint64_t t2 = perf_get_time_us();
        mu_assert_int_eq(0, msg_decode(buf, buf_len, &d));
        uint64_t t3 = perf_get_time_us();
        latencies_dec[i] = t3 - t2;
        latencies_total[i] = t3 - t0;

        msg_destroy(d);
        msg_free_buffer(buf);
    }
    uint64_t elapsed = perf_get_time_us() - start;

    /* 排序 */
    qsort(latencies_enc, iterations, sizeof(uint64_t), cmp_uint64);
    qsort(latencies_dec, iterations, sizeof(uint64_t), cmp_uint64);
    qsort(latencies_total, iterations, sizeof(uint64_t), cmp_uint64);

    /* 计算总字节数和吞吐率 */
    size_t total_bytes = 0;
    for (int i = 0; i < iterations; i++) total_bytes += sizes[i];
    double mbps = total_bytes * 1000000.0 / elapsed / (1024.0 * 1024.0);

    double qps = iterations * 1000000.0 / elapsed;
    double p50_enc = percentile(latencies_enc, iterations, 50.0);
    double p90_enc = percentile(latencies_enc, iterations, 90.0);
    double p50_dec = percentile(latencies_dec, iterations, 50.0);
    double p90_dec = percentile(latencies_dec, iterations, 90.0);
    double p99_total = percentile(latencies_total, iterations, 99.0);
    double p999_total = percentile(latencies_total, iterations, 99.9);

    printf("\n  [perf] encode_decode: %d iterations in %.3f ms\n", iterations, elapsed / 1000.0);
    printf("  [perf]   QPS: %.0f ops/s\n", qps);
    printf("  [perf]   throughput: %.2f MB/s\n", mbps);
    printf("  [perf]   encode:  p50=%.1f us  p90=%.1f us\n", p50_enc, p90_enc);
    printf("  [perf]   decode:  p50=%.1f us  p90=%.1f us\n", p50_dec, p90_dec);
    printf("  [perf]   total:   p99=%.1f us  p999=%.1f us\n", p99_total, p999_total);

    free(latencies_enc);
    free(latencies_dec);
    free(latencies_total);
    free(sizes);
    msg_destroy(p);
    mu_check(elapsed < 30000000ULL);  /* 30s 超时保护 */
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
    mu_check(msg_fetch_next(decoded));

    /* 验证解码内容正确 */
    mu_assert_str_eq("ETHUSD", get_row_value_by_key(decoded, "Symbol"));
    mu_assert_str_eq("3000.75", get_row_value_by_key(decoded, "Price"));

    /* 预热 */
    msg_packet_t *c = msg_clone(decoded);
    mu_check(c != NULL);
    msg_destroy(c);

    /* 正式计时 */
    uint64_t *latencies = (uint64_t *)malloc(sizeof(uint64_t) * iterations);
    mu_check(latencies != NULL);

    uint64_t start = perf_get_time_us();
    for (int i = 0; i < iterations; i++) {
        uint64_t t0 = perf_get_time_us();
        c = msg_clone(decoded);
        uint64_t t1 = perf_get_time_us();
        latencies[i] = t1 - t0;
        mu_check(c != NULL);
        msg_destroy(c);
    }
    uint64_t elapsed = perf_get_time_us() - start;

    /* 排序 */
    qsort(latencies, iterations, sizeof(uint64_t), cmp_uint64);

    double qps = iterations * 1000000.0 / elapsed;
    double p50 = percentile(latencies, iterations, 50.0);
    double p90 = percentile(latencies, iterations, 90.0);
    double p99 = percentile(latencies, iterations, 99.0);
    double p999 = percentile(latencies, iterations, 99.9);

    printf("\n  [perf] clone: %d iterations in %.3f ms\n", iterations, elapsed / 1000.0);
    printf("  [perf]   QPS: %.0f ops/s\n", qps);
    printf("  [perf]   latencies: p50=%.1f us  p90=%.1f us  p99=%.1f us  p999=%.1f us\n",
           p50, p90, p99, p999);

    free(latencies);
    msg_destroy(decoded);
    msg_free_buffer(buf);
    mu_check(elapsed < 30000000ULL);  /* 30s 超时保护 */
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
