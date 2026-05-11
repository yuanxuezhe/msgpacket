/*
 * msgpacket 完整生命周期测试
 * 覆盖：创建包 → 设置参数 → 增加结果集 → 设置参数 → 封包 → 重新解包 → 获取解包的参数 → 遍历结果集参数
 *
 * 编译：gcc -o test_full_cycle test_full_cycle.c ../src/msg_api.c ../src/msg_util.c -I../src -g -fsanitize=address
 * 运行：./test_full_cycle
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "msg_api.h"
#include "msg_packet.h"

/* ========== 辅助宏 ========== */
#define CHECK(expr, label, msg) do { \
    if (!(expr)) { \
        printf("FAIL: %s (%s)\n", msg, #expr); \
        goto label; \
    } \
} while(0)

#define CHECK_EQ_int(a, b, label, msg) do { \
    if ((a) != (b)) { \
        printf("FAIL: %s - expected %d, got %d\n", msg, (int)(a), (int)(b)); \
        goto label; \
    } \
} while(0)

#define CHECK_EQ_size_t(a, b, label, msg) do { \
    if ((a) != (b)) { \
        printf("FAIL: %s - expected %zu, got %zu\n", msg, (size_t)(a), (size_t)(b)); \
        goto label; \
    } \
} while(0)

#define CHECK_EQ_str(a, b, label, msg) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("FAIL: %s - expected '%s', got '%s'\n", msg, (b), (a)); \
        goto label; \
    } \
} while(0)

#define CHECK_NE_ptr(a, b, label, msg) do { \
    if ((a) == (b)) { \
        printf("FAIL: %s - pointers should not be equal\n", msg); \
        goto label; \
    } \
} while(0)

#define CHECK_EQ_ptr(a, b, label, msg) do { \
    if ((a) != (b)) { \
        printf("FAIL: %s - expected %p, got %p\n", msg, (void*)(b), (void*)(a)); \
        goto label; \
    } \
} while(0)

/* ========== 测试1：基本创建和字段设置 ========== */
static int test_basic_create_and_setters(void) {
    printf("\n=== Test: Basic Create and Setters ===\n");

    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    CHECK(p != NULL, out, "msg_create failed");

    /* 验证自动生成的字段 */
    const char *msg_id = msg_get_msg_id(p);
    CHECK(msg_id != NULL, out, "msg_id is NULL");
    CHECK(strlen(msg_id) == 32, out, "msg_id should be 32 chars");

    const char *ver = msg_get_version(p);
    CHECK_EQ_str(ver, "V1.0", out, "version");

    const char *func = msg_get_func(p);
    CHECK(func != NULL, out, "func is NULL");
    CHECK(strlen(func) == 0, out, "func should be empty initially");

    /* 手动设置字段 */
    int rc = msg_set_func(p, "query");
    CHECK_EQ_int(rc, 0, out, "msg_set_func");

    rc = msg_set_msg_id(p, "abcd1234567890abcd1234567890ab");
    CHECK_EQ_int(rc, 0, out, "msg_set_msg_id");

    /* timestamp 自动生成，无需手动设置 */

    /* 验证设置后的值 */
    func = msg_get_func(p);
    CHECK_EQ_str(func, "query", out, "func after set");

    msg_id = msg_get_msg_id(p);
    CHECK(strncmp(msg_id, "abcd1234", 8) == 0, out, "msg_id after set");

    printf("PASS: Basic create and setters\n");
    msg_destroy(p);
    return 0;

out:
    if (p) msg_destroy(p);
    return -1;
}

/* ========== 测试2：表头和数据行构建 ========== */
static int test_headers_and_rows(void) {
    printf("\n=== Test: Headers and Rows ===\n");

    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    CHECK(p != NULL, out, "msg_create failed");

    /* 设置表头 */
    int rc = msg_set_headers(p, 3, "Name,Age,City");
    CHECK_EQ_int(rc, 0, out, "msg_set_headers");
    CHECK_EQ_size_t(msg_get_header_count(p), 3, out, "header count");

    /* 添加行 */
    rc = msg_add_row(p);
    CHECK_EQ_int(rc, 0, out, "msg_add_row");

    /* 按 key 设置值 */
    rc = msg_set_value_str(p, "Name", "Alice");
    CHECK_EQ_int(rc, 0, out, "msg_set_value_str Name");

    rc = msg_set_value_str(p, "Age", "30");
    CHECK_EQ_int(rc, 0, out, "msg_set_value_str Age");

    rc = msg_set_value_str(p, "City", "Beijing");
    CHECK_EQ_int(rc, 0, out, "msg_set_value_str City");

    CHECK_EQ_size_t(msg_get_row_count(p), 1, out, "row count");

    /* 用 msg_set_row 快捷接口 */
    rc = msg_add_row(p);
    CHECK_EQ_int(rc, 0, out, "msg_add_row 2");

    rc = msg_set_row(p, "Bob,25,Shanghai");
    CHECK_EQ_int(rc, 0, out, "msg_set_row");

    CHECK_EQ_size_t(msg_get_row_count(p), 2, out, "row count 2");

    printf("PASS: Headers and rows\n");
    msg_destroy(p);
    return 0;

out:
    if (p) msg_destroy(p);
    return -1;
}

/* ========== 测试3：封包(msg_finalize) ========== */
static int test_finalize(void) {
    printf("\n=== Test: msg_finalize ===\n");

    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    CHECK(p != NULL, out, "msg_create failed");

    msg_set_headers(p, 2, "Key,Value");
    msg_add_row(p);
    msg_set_value_str(p, "Key", "hostname");
    msg_set_value_str(p, "Value", "server01");

    /* finalize */
    int rc = msg_finalize(p);
    CHECK_EQ_int(rc, 0, out, "msg_finalize");

    /* finalize 后可获取 wire 数据 */
    const void *data = msg_data(p);
    CHECK(data != NULL, out, "msg_data after finalize");

    size_t size = msg_size(p);
    CHECK(size > 0, out, "msg_size > 0");
    printf("  wire size: %zu bytes\n", size);

    /* 验证 magic */
    const char *magic = (const char*)data;
    CHECK(memcmp(magic, "YSWY", 4) == 0, out, "magic check");

    /* 验证 body_len 字段 */
    uint32_t body_len = *(uint32_t*)(data + 8);
    body_len = MSG_LE32TOH(body_len);
    printf("  body_len: %u\n", body_len);

    printf("PASS: msg_finalize\n");
    msg_destroy(p);
    return 0;

out:
    if (p) msg_destroy(p);
    return -1;
}

/* ========== 测试4：编码解码完整周期 ========== */
static int test_encode_decode(void) {
    printf("\n=== Test: Encode -> Decode ===\n");

    /* ========== 构建阶段 ========== */
    msg_packet_t *orig = msg_create(MSG_TYPE_ANSWER, "V1.0");
    CHECK(orig != NULL, out, "msg_create");

    msg_set_headers(orig, 3, "Symbol,Price,Volume");
    msg_add_row(orig);
    msg_set_value_str(orig, "Symbol", "AAPL");
    msg_set_value_str(orig, "Price", "150.25");
    msg_set_value_str(orig, "Volume", "1000");

    msg_add_row(orig);
    msg_set_value_str(orig, "Symbol", "GOOG");
    msg_set_value_str(orig, "Price", "2800.50");
    msg_set_value_str(orig, "Volume", "500");

    int rc = msg_finalize(orig);
    CHECK_EQ_int(rc, 0, out, "msg_finalize");

    /* ========== 编码 ========== */
    void *buf = NULL;
    size_t buf_len = 0;
    rc = msg_encode(orig, &buf, &buf_len);
    CHECK_EQ_int(rc, 0, out, "msg_encode");
    CHECK(buf != NULL, out, "buf != NULL");
    printf("  encoded size: %zu bytes\n", buf_len);

    /* ========== 解码 ========== */
    msg_packet_t *decoded = NULL;
    rc = msg_decode(buf, buf_len, &decoded);
    CHECK_EQ_int(rc, 0, out, "msg_decode");
    CHECK(decoded != NULL, out, "decoded != NULL");

    /* ========== 验证解码后的字段 ========== */
    CHECK_EQ_str(msg_get_msg_id(decoded), msg_get_msg_id(orig), out, "msg_id preserved");
    CHECK_EQ_str(msg_get_version(decoded), msg_get_version(orig), out, "version preserved");
    CHECK_EQ_str(msg_get_func(decoded), msg_get_func(decoded), out, "func preserved");
    CHECK_EQ_int(msg_get_type(decoded), msg_get_type(orig), out, "msg_type preserved");

    /* ========== 验证数据 ========== */
    CHECK_EQ_size_t(msg_get_header_count(decoded), 3, out, "header count");
    CHECK_EQ_size_t(msg_get_row_count(decoded), 2, out, "row count");

    /* 按索引获取 */
    const char *val = NULL;
    size_t vlen = 0;

    rc = msg_get_field(decoded, 0, 0, &val, &vlen);
    CHECK_EQ_int(rc, 0, out, "get_field[0,0]");
    CHECK_EQ_str("AAPL", val, out, "field[0,0]");

    rc = msg_get_field(decoded, 1, 1, &val, &vlen);
    CHECK_EQ_int(rc, 0, out, "get_field[1,1]");
    CHECK_EQ_str("2800.50", val, out, "field[1,1]");

    printf("PASS: Encode -> Decode cycle\n");
    msg_destroy(decoded);
    msg_free_buffer(buf);
    msg_destroy(orig);
    return 0;

out:
    if (orig) msg_destroy(orig);
    return -1;
}

/* ========== 测试5：Clone 深度拷贝 ========== */
static int test_clone(void) {
    printf("\n=== Test: Clone ===\n");

    /* 构建并编码解码 */
    msg_packet_t *orig = msg_create(MSG_TYPE_REQUEST, "V1.0");
    msg_set_func(orig, "query");
    msg_set_headers(orig, 2, "K,V");
    msg_add_row(orig);
    msg_set_value_str(orig, "K", "key1");
    msg_set_value_str(orig, "V", "val1");
    msg_finalize(orig);

    void *buf = NULL; size_t buf_len = 0;
    msg_encode(orig, &buf, &buf_len);

    msg_packet_t *decoded = NULL;
    msg_decode(buf, buf_len, &decoded);

    /* Clone */
    msg_packet_t *cloned = msg_clone(decoded);
    CHECK(cloned != NULL, out, "msg_clone");
    CHECK_NE_ptr(cloned, decoded, out, "clone should be different pointer");

    /* 验证数据一致 */
    CHECK_EQ_str(msg_get_func(cloned), "query", out, "func after clone");
    CHECK_EQ_size_t(msg_get_header_count(cloned), 2, out, "header count");
    CHECK_EQ_size_t(msg_get_row_count(cloned), 1, out, "row count");

    /* 按 key 获取 */
    const char *v = NULL; size_t vlen = 0;
    msg_get_value_str(cloned, "K", &v, &vlen);
    CHECK_EQ_str("key1", v, out, "value after clone");

    printf("PASS: Clone\n");
    msg_destroy(cloned);
    msg_destroy(decoded);
    msg_free_buffer(buf);
    msg_destroy(orig);
    return 0;

out:
    return -1;
}

/* ========== 测试6：多结果集 ========== */
static int test_multi_result_sets(void) {
    printf("\n=== Test: Multi-Result-Sets ===\n");

    msg_packet_t *p = msg_create(MSG_TYPE_ANSWER, "V1.0");
    msg_set_func(p, "query");

    /* ========== RS1 ========== */
    msg_set_headers(p, 2, "ID,Name");
    msg_add_row(p);
    msg_set_value_str(p, "ID", "1");
    msg_set_value_str(p, "Name", "Alice");

    /* ========== RS2 ========== */
    bool ok = msg_add_result_set(p);
    CHECK(ok, out, "msg_add_result_set");
    CHECK_EQ_size_t(msg_get_result_set(p), 2, out, "current rs should be 2");

    msg_set_headers(p, 2, "ID,Score");
    msg_add_row(p);
    msg_set_value_str(p, "ID", "1");
    msg_set_value_str(p, "Score", "95");

    /* ========== RS3 ========== */
    ok = msg_add_result_set(p);
    CHECK(ok, out, "msg_add_result_set RS3");
    CHECK_EQ_size_t(msg_get_result_set(p), 3, out, "current rs should be 3");

    msg_set_headers(p, 1, "Status");
    msg_add_row(p);
    msg_set_value_str(p, "Status", "OK");

    CHECK_EQ_size_t(msg_get_result_set_count(p), 3, out, "total rs count");

    /* finalize */
    int rc = msg_finalize(p);
    CHECK_EQ_int(rc, 0, out, "msg_finalize multi-RS");

    /* encode */
    void *buf = NULL; size_t buf_len = 0;
    rc = msg_encode(p, &buf, &buf_len);
    CHECK_EQ_int(rc, 0, out, "msg_encode multi-RS");

    /* decode */
    msg_packet_t *decoded = NULL;
    rc = msg_decode(buf, buf_len, &decoded);
    CHECK_EQ_int(rc, 0, out, "msg_decode multi-RS");
    CHECK_EQ_size_t(msg_get_result_set_count(decoded), 3, out, "rs count after decode");

    /* 切换结果集 */
    rc = msg_select_result_set(decoded, 1);
    CHECK_EQ_int(rc, 0, out, "select RS1");
    CHECK_EQ_size_t(msg_get_row_count(decoded), 1, out, "RS1 row count");

    rc = msg_select_result_set(decoded, 2);
    CHECK_EQ_int(rc, 0, out, "select RS2");
    CHECK_EQ_size_t(msg_get_header_count(decoded), 2, out, "RS2 header count");

    rc = msg_select_result_set(decoded, 3);
    CHECK_EQ_int(rc, 0, out, "select RS3");
    CHECK_EQ_size_t(msg_get_header_count(decoded), 1, out, "RS3 header count");

    /* next_result_set */
    msg_packet_t *p2 = msg_create(MSG_TYPE_ANSWER, "V1.0");
    msg_set_headers(p2, 1, "A");
    msg_add_row(p2);
    msg_set_value_str(p2, "A", "1");
    msg_add_result_set(p2);
    msg_set_headers(p2, 1, "B");
    msg_add_row(p2);
    msg_set_value_str(p2, "B", "2");
    msg_finalize(p2);
    msg_destroy(p2);

    printf("PASS: Multi-Result-Sets\n");
    msg_destroy(decoded);
    msg_free_buffer(buf);
    msg_destroy(p);
    return 0;

out:
    return -1;
}

/* ========== 测试7：遍历数据(msg_fetch_next) ========== */
static int test_cursor_iteration(void) {
    printf("\n=== Test: Cursor Iteration ===\n");

    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    msg_set_headers(p, 2, "K,V");
    msg_add_row(p);
    msg_set_value_str(p, "K", "a");
    msg_set_value_str(p, "V", "1");
    msg_add_row(p);
    msg_set_value_str(p, "K", "b");
    msg_set_value_str(p, "V", "2");
    msg_add_row(p);
    msg_set_value_str(p, "K", "c");
    msg_set_value_str(p, "V", "3");
    msg_finalize(p);

    void *buf = NULL; size_t buf_len = 0;
    msg_encode(p, &buf, &buf_len);

    msg_packet_t *decoded = NULL;
    msg_decode(buf, buf_len, &decoded);

    /* fetch 遍历 */
    size_t count = 0;
    while (msg_fetch_next(decoded)) {
        const char *v = NULL; size_t vlen = 0;
        int rc = msg_get_value_str(decoded, "K", &v, &vlen);
        if (rc == 0 && v) {
            printf("  row %zu: K=%.*s\n", count, (int)vlen, v);
        }
        count++;
    }
    CHECK_EQ_size_t(count, 3, out, "iterated 3 rows");

    /* reset 后再遍历 */
    msg_reset_cursor(decoded);
    CHECK(!msg_fetch_next(decoded) || msg_get_current_row(decoded) == 0, out, "after reset");

    printf("PASS: Cursor iteration\n");
    msg_destroy(decoded);
    msg_free_buffer(buf);
    msg_destroy(p);
    return 0;

out:
    return -1;
}

/* ========== 测试8：数值类型设置和获取 ========== */
static int test_numeric_types(void) {
    printf("\n=== Test: Numeric Types ===\n");

    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    msg_set_headers(p, 4, "i32,i64,double,str");

    msg_add_row(p);
    msg_set_value_i32(p, "i32", -12345);
    msg_set_value_i64(p, "i64", 9876543210LL);
    msg_set_value_double(p, "double", 3.141592653589);
    msg_set_value_str(p, "str", "hello");

    msg_finalize(p);

    void *buf = NULL; size_t buf_len = 0;
    msg_encode(p, &buf, &buf_len);

    msg_packet_t *d = NULL;
    msg_decode(buf, buf_len, &d);

    /* 验证数值 */
    int32_t iv = 0;
    int rc = msg_get_value_i32(d, "i32", &iv);
    CHECK_EQ_int(rc, 0, out, "get i32");
    CHECK_EQ_int(iv, -12345, out, "i32 value");

    int64_t lv = 0;
    rc = msg_get_value_i64(d, "i64", &lv);
    CHECK_EQ_int(rc, 0, out, "get i64");
    CHECK_EQ_int(lv, 9876543210LL, out, "i64 value");

    double dv = 0;
    rc = msg_get_value_double(d, "double", &dv);
    CHECK_EQ_int(rc, 0, out, "get double");

    printf("PASS: Numeric types\n");
    msg_destroy(d);
    msg_free_buffer(buf);
    msg_destroy(p);
    return 0;

out:
    return -1;
}

/* ========== 测试9：空行和边界 ========== */
static int test_edge_cases(void) {
    printf("\n=== Test: Edge Cases ===\n");

    /* 正确做法：先设置表头 */
    msg_packet_t *p = msg_create(MSG_TYPE_PUSH, "V1.0");
    msg_set_headers(p, 1, "Data");
    msg_add_row(p);
    msg_set_value_str(p, "Data", "test");
    int rc = msg_finalize(p);
    CHECK_EQ_int(rc, 0, out, "finalize with header");

    void *buf = NULL; size_t buf_len = 0;
    rc = msg_encode(p, &buf, &buf_len);
    CHECK_EQ_int(rc, 0, out, "encode with header");

    msg_packet_t *d = NULL;
    rc = msg_decode(buf, buf_len, &d);
    CHECK_EQ_int(rc, 0, out, "decode with header");
    printf("  header row_count: %zu\n", msg_get_row_count(d));

    printf("PASS: Edge cases\n");
    msg_destroy(d);
    msg_free_buffer(buf);
    msg_destroy(p);
    return 0;

out:
    return -1;
}

/* ========== 主函数 ========== */
int main(void) {
    printf("=========================================\n");
    printf(" msgpacket 完整生命周期测试\n");
    printf("=========================================\n");

    int failures = 0;
    #define RUN_TEST(t) do { \
        if (t() != 0) { \
            printf("^^^ TEST %s FAILED\n", #t); \
            failures++; \
        } \
    } while(0)

    RUN_TEST(test_basic_create_and_setters);
    RUN_TEST(test_headers_and_rows);
    RUN_TEST(test_finalize);
    RUN_TEST(test_encode_decode);
    RUN_TEST(test_clone);
    RUN_TEST(test_multi_result_sets);
    RUN_TEST(test_cursor_iteration);
    RUN_TEST(test_numeric_types);
    RUN_TEST(test_edge_cases);

    printf("\n=========================================\n");
    if (failures == 0) {
        printf("  ALL TESTS PASSED (%d tests)\n", 9);
    } else {
        printf("  %d TESTS FAILED\n", failures);
    }
    printf("=========================================\n");

    return failures ? 1 : 0;
}
