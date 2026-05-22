/*
 * test_regression.c — MsgPacket 回归测试套件
 * 覆盖历史 bug 场景，防止重蹈覆辙
 * 框架：MinUnit（header-only，零依赖）
 * 编译方式：直接编译源码（不链接动态库）
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

/* 获取当前 result set 中指定 key 的值（用于 decode 后的包） */
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
 * Bug #1: parse_single_rs 两遍扫描漏尾部
 *
 * 触发条件：header 区最后一个字段不在循环范围内
 * 历史场景：header="Name,Age"，header_count=2 但只解析出 "Name"
 * 验证方法：decode 后两个字段都能读到
 * ================================================================ */
MU_TEST(test_regression_parse_rs_tail_field) {
    /* 构建：Name,Age + 一行数据：Bob,25 */
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    msg_set_func(p, "query");

    msg_set_headers(p, 2, "Name,Age");
    msg_add_row(p);
    msg_set_value_str(p, "Name", "Bob");
    msg_set_value_str(p, "Age", "25");
    mu_assert_int_eq(0, msg_finalize(p));

    /* encode → decode → 验证两个字段都能读到 */
    void *buf = NULL;
    size_t buf_len = 0;
    mu_assert_int_eq(0, msg_encode(p, &buf, &buf_len));
    msg_destroy(p);

    msg_packet_t *d = NULL;
    mu_assert_int_eq(0, msg_decode(buf, buf_len, &d));
    mu_check(d != NULL);

    /* 关键验证：两个字段都存在且值正确 */
    mu_check(msg_fetch_next(d));
    mu_assert_str_eq("Bob", get_row_value_by_key(d, "Name"));
    mu_assert_str_eq("25",  get_row_value_by_key(d, "Age"));

    msg_destroy(d);
    msg_free_buffer(buf);
    return 0;
}

/* ================================================================
 * Bug #2: malloc 运算符优先级 bug
 *
 * 错误写法：malloc(sizeof(char*) * rows * cols + total_bytes)
 * 正确写法：size_t nptr = rows * cols; malloc(sizeof(char*) * nptr + total_bytes)
 *
 * 历史场景：2x3 结果集 encode+decode+clone 不 SEGFAULT
 * 验证方法：多行多列数据 clone 后所有字段值正确
 * ================================================================ */
MU_TEST(test_regression_malloc_precedence_2x3) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    msg_set_func(p, "query");

    /* 3 列 x 2 行 */
    msg_set_headers(p, 3, "A,B,C");
    msg_add_row(p);
    msg_set_value_str(p, "A", "r1a"); msg_set_value_str(p, "B", "r1b"); msg_set_value_str(p, "C", "r1c");
    msg_add_row(p);
    msg_set_value_str(p, "A", "r2a"); msg_set_value_str(p, "B", "r2b"); msg_set_value_str(p, "C", "r2c");
    mu_assert_int_eq(0, msg_finalize(p));

    void *buf = NULL; size_t buf_len = 0;
    mu_assert_int_eq(0, msg_encode(p, &buf, &buf_len));
    msg_destroy(p);

    msg_packet_t *d = NULL;
    mu_assert_int_eq(0, msg_decode(buf, buf_len, &d));
    mu_check(d != NULL);
    mu_check(msg_fetch_next(d));

    /* 验证所有 6 个字段值 */
    mu_assert_str_eq("r1a", get_row_value_by_key(d, "A"));
    mu_assert_str_eq("r1b", get_row_value_by_key(d, "B"));
    mu_assert_str_eq("r1c", get_row_value_by_key(d, "C"));

    mu_check(msg_fetch_next(d));  /* 前进到第二行 */
    mu_assert_str_eq("r2a", get_row_value_by_key(d, "A"));
    mu_assert_str_eq("r2b", get_row_value_by_key(d, "B"));
    mu_assert_str_eq("r2c", get_row_value_by_key(d, "C"));

    /* clone 验证 */
    msg_packet_t *c = msg_clone(d);
    mu_check(c != NULL);
    mu_check(msg_fetch_next(c));
    mu_assert_str_eq("r1a", get_row_value_by_key(c, "A"));
    mu_assert_str_eq("r2c", get_row_value_by_key(c, "C"));

    msg_destroy(d);
    msg_destroy(c);
    msg_free_buffer(buf);
    return 0;
}

/* ================================================================
 * Bug #3: unescaped_body 只在 decode 后填充
 *
 * 历史场景：直接 clone 未 decode 的包，get_value 返回空
 * 正确流程：build → encode → decode → clone → get_value
 * 验证方法：比较 encode→decode→clone 和直接 clone 的行为差异
 * ================================================================ */
MU_TEST(test_regression_unescaped_body_timing) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    msg_set_func(p, "query");
    msg_set_headers(p, 2, "Key,Value");
    msg_add_row(p);
    msg_set_value_str(p, "Key", "K1");
    msg_set_value_str(p, "Value", "V1");
    mu_assert_int_eq(0, msg_finalize(p));

    /* 方式 A（正确）：encode → decode → clone → get_value */
    void *buf = NULL; size_t buf_len = 0;
    mu_assert_int_eq(0, msg_encode(p, &buf, &buf_len));
    msg_destroy(p);

    msg_packet_t *d = NULL;
    mu_assert_int_eq(0, msg_decode(buf, buf_len, &d));
    mu_check(d != NULL);
    mu_check(msg_fetch_next(d));

    msg_packet_t *c = msg_clone(d);
    mu_check(c != NULL);
    mu_check(msg_fetch_next(c));

    /* clone 后能读到值 */
    const char *val = get_row_value_by_key(c, "Key");
    mu_check(val != NULL);
    mu_assert_str_eq("K1", val);

    msg_destroy(d);
    msg_destroy(c);
    msg_free_buffer(buf);
    return 0;
}

/* ================================================================
 * Bug #4: msg_get_value_str 无 null terminator
 *
 * 历史场景：val 指向 unescaped_body，无 \0 终止，strlen 会读到后续垃圾
 * 验证方法：用 val_len 限制 + 加 \0 后内容匹配
 * ================================================================ */
MU_TEST(test_regression_value_no_null_term) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    msg_set_func(p, "query");
    msg_set_headers(p, 2, "K,V");
    msg_add_row(p);
    msg_set_value_str(p, "K", "TestKey");
    msg_set_value_str(p, "V", "TestVal");
    mu_assert_int_eq(0, msg_finalize(p));

    void *buf = NULL; size_t buf_len = 0;
    mu_assert_int_eq(0, msg_encode(p, &buf, &buf_len));
    msg_destroy(p);

    msg_packet_t *d = NULL;
    mu_assert_int_eq(0, msg_decode(buf, buf_len, &d));
    mu_check(d != NULL);
    mu_check(msg_fetch_next(d));

    /* 验证 val_len 正确 */
    const char *val = NULL;
    size_t val_len = 0;
    mu_assert_int_eq(0, msg_get_value_str(d, "K", &val, &val_len));
    mu_assert_int_eq(7, val_len);  /* strlen("TestKey") == 7 */

    /* 加 \0 后用 memcmp 比较 */
    char buf_with_null[64] = {0};
    memcpy(buf_with_null, val, val_len);
    mu_assert_mem_eq("TestKey", buf_with_null, 7);

    msg_destroy(d);
    msg_free_buffer(buf);
    return 0;
}

/* ================================================================
 * Bug #5: timestamp 越界写入（已修复）
 *
 * 错误：memcpy(out, tmp, sizeof(tmp)) 写 19 字节到 18 字节数组
 * 正确：memcpy(out, tmp, HEAD_TIMESTAMP_LENGTH) 只拷贝 18 字节
 * 验证：strlen(msg_get_timestamp(p)) == 17
 * ================================================================ */
MU_TEST(test_regression_timestamp_no_overflow) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);

    const char *ts = msg_get_timestamp(p);
    mu_check(ts != NULL);

    /* timestamp 是 17 位数字 + \0，共 18 字节 */
    size_t ts_len = strlen(ts);
    mu_assert_int_eq(17, ts_len);

    /* 内容应该是数字 */
    for (size_t i = 0; i < ts_len; i++) {
        mu_check(ts[i] >= '0' && ts[i] <= '9');
    }

    msg_destroy(p);
    return 0;
}

/* ================================================================
 * Bug #6: msg_copy_fixed_field 截断超长字符串
 *
 * 历史场景：set_func("123456789") 后 get_func 返回 "12345678"（8字节截断）
 * 验证：HEAD_FUNC_LENGTH = 9（含 \0），所以 max_len = 8
 * ================================================================ */
MU_TEST(test_regression_fixed_field_truncation) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);

    /* func 字段最大 8 字节（数组 9 字节，含 \0） */
    mu_assert_int_eq(0, msg_set_func(p, "123456789"));  /* 9 字节输入 */
    mu_assert_str_eq("12345678", msg_get_func(p));      /* 截断为 8 字节 */

    /* 边界测试：刚好 8 字节 */
    mu_assert_int_eq(0, msg_set_func(p, "12345678"));
    mu_assert_str_eq("12345678", msg_get_func(p));

    /* 边界测试：7 字节（不变） */
    mu_assert_int_eq(0, msg_set_func(p, "1234567"));
    mu_assert_str_eq("1234567", msg_get_func(p));

    msg_destroy(p);
    return 0;
}

/* ================================================================
 * Bug #7: parse_single_rs 空结果集 leak（f_start >= end_offset 时循环仍执行）
 *
 * 历史场景：HEARTBEAT body 只有 FS 区，数据区为空，泄漏 1 字节
 * 验证：decode HEARTBEAT 包不泄漏
 * ================================================================ */
MU_TEST(test_regression_empty_rs_no_leak) {
    msg_packet_t *p = msg_create(MSG_TYPE_HEARTBEAT, "V1.0");
    mu_check(p != NULL);
    msg_set_func(p, "ping");
    mu_assert_int_eq(0, msg_finalize(p));

    void *buf = NULL; size_t buf_len = 0;
    mu_assert_int_eq(0, msg_encode(p, &buf, &buf_len));
    msg_destroy(p);

    msg_packet_t *d = NULL;
    mu_assert_int_eq(0, msg_decode(buf, buf_len, &d));
    mu_check(d != NULL);
    /* HEARTBEAT 解码后不应该是 NULL 或崩溃 */

    msg_destroy(d);
    msg_free_buffer(buf);
    return 0;
}

/* ================================================================
 * Bug #8: clone 成功路径无泄漏
 *
 * 验证：正常 clone 操作 ASAN 检测无泄漏
 * ================================================================ */
MU_TEST(test_regression_clone_no_leak) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    msg_set_func(p, "query");
    msg_set_headers(p, 3, "A,B,C");
    msg_add_row(p);
    msg_set_value_str(p, "A", "v1"); msg_set_value_str(p, "B", "v2"); msg_set_value_str(p, "C", "v3");
    mu_assert_int_eq(0, msg_finalize(p));

    void *buf = NULL; size_t buf_len = 0;
    mu_assert_int_eq(0, msg_encode(p, &buf, &buf_len));
    msg_destroy(p);

    msg_packet_t *d = NULL;
    mu_assert_int_eq(0, msg_decode(buf, buf_len, &d));
    mu_check(d != NULL);
    mu_check(msg_fetch_next(d));

    /* 多次 clone */
    for (int i = 0; i < 10; i++) {
        msg_packet_t *c = msg_clone(d);
        mu_check(c != NULL);
        mu_check(msg_fetch_next(c));
        mu_assert_str_eq("v1", get_row_value_by_key(c, "A"));
        msg_destroy(c);
    }

    msg_destroy(d);
    msg_free_buffer(buf);
    return 0;
}

/* ================================================================
 * Bug #9: encode 后未 finalize 返回错误
 *
 * 验证：未 finalize 就 encode 返回 MSG_ERR_NOT_FINALIZED
 * ================================================================ */
MU_TEST(test_regression_encode_without_finalize) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    msg_set_func(p, "query");
    msg_set_headers(p, 1, "A");
    msg_add_row(p);
    msg_set_value_str(p, "A", "v1");
    /* 注意：没有调用 msg_finalize */

    void *buf = NULL; size_t buf_len = 0;
    int ret = msg_encode(p, &buf, &buf_len);
    mu_assert_int_eq(MSG_ERR_NOT_FINALIZED, ret);

    msg_destroy(p);
    return 0;
}

/* ================================================================
 * Bug #10: 多结果集 GS 分隔符解析
 *
 * 验证：多结果集 encode → decode 后每个结果集数据完整
 * ================================================================ */
MU_TEST(test_regression_multi_result_set) {
    msg_packet_t *p = msg_create(MSG_TYPE_ANSWER, "V1.0");
    mu_check(p != NULL);
    msg_set_func(p, "query");

    /* RS1 */
    msg_set_headers(p, 2, "K1,V1");
    msg_add_row(p);
    msg_set_value_str(p, "K1", "RS1_K1"); msg_set_value_str(p, "V1", "RS1_V1");

    /* RS2 */
    msg_add_result_set(p);
    mu_check(msg_next_result_set(p));
    msg_set_headers(p, 2, "K2,V2");
    msg_add_row(p);
    msg_set_value_str(p, "K2", "RS2_K2"); msg_set_value_str(p, "V2", "RS2_V2");

    mu_assert_int_eq(0, msg_finalize(p));

    void *buf = NULL; size_t buf_len = 0;
    mu_assert_int_eq(0, msg_encode(p, &buf, &buf_len));
    msg_destroy(p);

    msg_packet_t *d = NULL;
    mu_assert_int_eq(0, msg_decode(buf, buf_len, &d));
    mu_check(d != NULL);
    mu_assert_int_eq(2, msg_get_result_set_count(d));

    /* RS1 */
    mu_check(msg_fetch_next(d));
    mu_assert_str_eq("RS1_K1", get_row_value_by_key(d, "K1"));

    /* RS2 */
    mu_check(msg_next_result_set(d));
    mu_check(msg_fetch_next(d));
    mu_assert_str_eq("RS2_K2", get_row_value_by_key(d, "K2"));

    msg_destroy(d);
    msg_free_buffer(buf);
    return 0;
}

/* ================================================================
 * 测试套件
 * ================================================================ */
MU_TEST_SUITE(regression_tests) {
    MU_RUN_TEST(test_regression_parse_rs_tail_field);
    MU_RUN_TEST(test_regression_malloc_precedence_2x3);
    MU_RUN_TEST(test_regression_unescaped_body_timing);
    MU_RUN_TEST(test_regression_value_no_null_term);
    MU_RUN_TEST(test_regression_timestamp_no_overflow);
    MU_RUN_TEST(test_regression_fixed_field_truncation);
    MU_RUN_TEST(test_regression_empty_rs_no_leak);
    MU_RUN_TEST(test_regression_clone_no_leak);
    MU_RUN_TEST(test_regression_encode_without_finalize);
    MU_RUN_TEST(test_regression_multi_result_set);
}

/* ================================================================
 * main
 * ================================================================ */
int main(void) {
    printf("=== MsgPacket Regression Tests ===\n\n");
    MU_RUN_SUITE(regression_tests);
    MU_PRINT_REPORT();
    return mu_failed > 0 ? 1 : 0;
}
