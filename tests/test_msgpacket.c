/*
 * test_msgpacket.c — MsgPacket 单元测试（MinUnit 框架，跨平台零依赖）
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "minunit.h"
#include "msg_api.h"
#include "msg_util.h"
#include "msg_packet.h"
#include "msg_byteorder.h"

/* ================================================================
 * 辅助函数
 * ================================================================ */

/* 获取当前 result set 中指定 key 的值 */
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
 * CRC32 标准向量验证
 * ================================================================ */
MU_TEST(test_crc32_known_vectors) {
    crc32_init();

    struct {
        const uint8_t *data;
        size_t len;
        uint32_t expected;
    } vectors[] = {
        {(uint8_t*)"", 0, 0x00000000},
        {(uint8_t*)"\x00\x00\x00\x00", 4, 0x2144DF1C},
        {(uint8_t*)"\xFF\xFF\xFF\xFF", 4, 0xFFFFFFFF},
        {(uint8_t*)"123456789", 9, 0xCBF43926},
    };

    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        uint32_t crc = crc32_update(0, vectors[i].data, vectors[i].len);
        mu_assert_int_eq(vectors[i].expected, crc);
    }
    return 0;
}

/* ================================================================
 * 转义/反转义
 * ================================================================ */
MU_TEST(test_escape_unescape_cycle) {
    /* 普通数据 */
    uint8_t raw[] = "hello world";
    size_t esc_len, unesc_len;
    uint8_t *esc = msg_escape(raw, sizeof(raw) - 1, &esc_len);
    mu_check(esc != NULL);
    uint8_t *unesc = msg_unescape(esc, esc_len, &unesc_len);
    mu_check(unesc != NULL);
    mu_assert_int_eq(sizeof(raw) - 1, unesc_len);
    mu_assert_mem_eq(raw, unesc, sizeof(raw) - 1);
    free(esc);
    free(unesc);

    /* 含转义字符数据（Unit Separator 0x1F） */
    uint8_t raw2[] = "a\x1Fb\x1Ec\x1Cd";
    uint8_t *esc2 = msg_escape(raw2, sizeof(raw2) - 1, &esc_len);
    uint8_t *unesc2 = msg_unescape(esc2, esc_len, &unesc_len);
    mu_check(unesc2 != NULL);
    mu_assert_int_eq(sizeof(raw2) - 1, unesc_len);
    mu_assert_mem_eq(raw2, unesc2, sizeof(raw2) - 1);
    free(esc2);
    free(unesc2);

    /* 含 ESC 本身（0x1B）后跟普通字符 */
    uint8_t raw3[] = "a\x1Bb";
    uint8_t *esc3 = msg_escape(raw3, sizeof(raw3) - 1, &esc_len);
    uint8_t *unesc3 = msg_unescape(esc3, esc_len, &unesc_len);
    mu_check(unesc3 != NULL);
    mu_assert_mem_eq(raw3, unesc3, sizeof(raw3) - 1);
    free(esc3);
    free(unesc3);

    /* 孤立 ESC → 必须返回 NULL */
    uint8_t bad[] = "\x1B";
    size_t out_len;
    mu_check(msg_unescape(bad, 1, &out_len) == NULL);

    /* 无效转义后缀 */
    uint8_t bad2[] = "\x1BX";
    mu_check(msg_unescape(bad2, 2, &out_len) == NULL);

    return 0;
}

/* ================================================================
 * 创建 / 销毁
 * ================================================================ */
MU_TEST(test_msg_create_destroy) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    mu_assert_int_eq(MSG_TYPE_REQUEST, msg_get_type(p));
    mu_assert_str_eq("V1.0", msg_get_version(p));
    mu_check(msg_get_msg_id(p) != NULL);
    mu_assert_int_eq(32, strlen(msg_get_msg_id(p)));
    msg_destroy(p);

    /* NULL 销毁不崩溃 */
    msg_destroy(NULL);
    return 0;
}

/* ================================================================
 * msg_clone 内存泄漏修复验证
 * ================================================================ */
MU_TEST(test_msg_clone_no_leak) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);

    int ret = msg_set_headers(p, 2, "Name,Age");
    mu_assert_int_eq(0, ret);

    ret = msg_add_row(p);
    mu_assert_int_eq(0, ret);

    ret = msg_set_value_str(p, "Name", "Alice");
    mu_assert_int_eq(0, ret);

    ret = msg_set_value_str(p, "Age", "30");
    mu_assert_int_eq(0, ret);

    mu_assert_int_eq(0, msg_finalize(p));

    /* 序列化再反序列化，确保 unescaped_body 有数据 */
    void *buf = NULL;
    size_t buf_len = 0;
    ret = msg_encode(p, &buf, &buf_len);
    mu_assert_int_eq(0, ret);
    mu_check(buf != NULL);
    mu_check(buf_len > 0);

    msg_packet_t *decoded = NULL;
    ret = msg_decode(buf, buf_len, &decoded);
    mu_assert_int_eq(0, ret);
    mu_check(decoded != NULL);

    /* 从已解码的包克隆（unescaped_body 已复制） */
    msg_packet_t *c = msg_clone(decoded);
    mu_check(c != NULL);
    mu_assert_str_eq(msg_get_msg_id(decoded), msg_get_msg_id(c));

    /* 验证克隆内容正确 */
    mu_assert_str_eq("Alice", get_row_value_by_key(c, "Name"));
    mu_assert_str_eq("30", get_row_value_by_key(c, "Age"));

    /* 销毁原包，克隆应不受影响 */
    msg_destroy(decoded);

    /* 验证克隆内容还在 */
    mu_assert_str_eq("Alice", get_row_value_by_key(c, "Name"));

    msg_destroy(c);
    msg_destroy(p);
    msg_free_buffer(buf);
    return 0;
}

/* ================================================================
 * Header 字段设置 / 获取
 * ================================================================ */
MU_TEST(test_msg_header_fields) {
    msg_packet_t *p = msg_create(MSG_TYPE_ANSWER, "V1.0");
    mu_check(p != NULL);

    mu_assert_int_eq(0, msg_set_func(p, "getUsr"));
    mu_assert_str_eq("getUsr", msg_get_func(p));

    mu_assert_int_eq(0, msg_set_timestamp(p, "20250505000000000"));
    mu_assert_str_eq("20250505000000000", msg_get_timestamp(p));

    mu_assert_int_eq(0, msg_set_format(p, MSG_FORMAT_TABLE));
    mu_assert_int_eq(MSG_FORMAT_TABLE, msg_get_format(p));

    msg_destroy(p);
    return 0;
}

/* ================================================================
 * 表头 / 数据行
 * ================================================================ */
MU_TEST(test_msg_headers_and_rows) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);

    mu_assert_int_eq(0, msg_set_headers(p, 3, "Symbol,Price,Volume"));

    char buf[256];
    size_t len = sizeof(buf);
    mu_assert_int_eq(0, msg_get_headers(p, buf, &len));
    mu_assert_str_eq("Symbol,Price,Volume", buf);

    mu_assert_int_eq(0, msg_add_row(p));
    mu_assert_int_eq(0, msg_set_value_str(p, "Symbol", "BTCUSD"));
    mu_assert_int_eq(0, msg_set_value_str(p, "Price", "50000"));
    mu_assert_int_eq(0, msg_set_value_str(p, "Volume", "100"));

    mu_assert_int_eq(0, msg_add_row(p));
    mu_assert_int_eq(0, msg_set_value_str(p, "Symbol", "ETHUSD"));
    mu_assert_int_eq(0, msg_set_value_str(p, "Price", "3000"));
    mu_assert_int_eq(0, msg_set_value_str(p, "Volume", "500"));

    mu_assert_int_eq(0, msg_clear_rows(p));

    msg_destroy(p);
    return 0;
}

MU_TEST(test_msg_numeric_values) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    mu_assert_int_eq(0, msg_set_headers(p, 4, "Id,I64,Dbl,Str"));
    mu_assert_int_eq(0, msg_add_row(p));
    mu_assert_int_eq(0, msg_set_value_i32(p, "Id", 12345));
    mu_assert_int_eq(0, msg_set_value_i64(p, "I64", -12345678901234LL));
    mu_assert_int_eq(0, msg_set_value_double(p, "Dbl", 3.14159));
    mu_assert_int_eq(0, msg_set_value_str(p, "Str", "test"));

    mu_assert_int_eq(0, msg_finalize(p));
    mu_check(msg_data(p) != NULL);
    mu_check(msg_size(p) > 0);

    msg_destroy(p);
    return 0;
}

/* ================================================================
 * 封包 / 解包 完整流程
 * ================================================================ */
MU_TEST(test_full_encode_decode_cycle) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    msg_set_func(p, "query");
    msg_set_headers(p, 2, "Key,Val");
    msg_add_row(p);
    msg_set_value_str(p, "Key", "hostname");
    msg_set_value_str(p, "Val", "server1");
    mu_assert_int_eq(0, msg_finalize(p));

    void *buf = NULL;
    size_t buf_len = 0;
    mu_assert_int_eq(0, msg_encode(p, &buf, &buf_len));
    mu_check(buf != NULL);
    mu_check(buf_len > 0);

    msg_packet_t *decoded = NULL;
    mu_assert_int_eq(0, msg_decode(buf, buf_len, &decoded));
    mu_check(decoded != NULL);

    mu_assert_int_eq(MSG_TYPE_REQUEST, msg_get_type(decoded));
    mu_assert_str_eq("query", msg_get_func(decoded));
    mu_assert_str_eq("V1.0", msg_get_version(decoded));

    char hdr_buf[64];
    size_t hdr_len = sizeof(hdr_buf);
    mu_assert_int_eq(0, msg_get_headers(decoded, hdr_buf, &hdr_len));
    mu_assert_str_eq("Key,Val", hdr_buf);

    mu_assert_str_eq("hostname", get_row_value_by_key(decoded, "Key"));
    mu_assert_str_eq("server1", get_row_value_by_key(decoded, "Val"));

    msg_free_buffer(buf);
    msg_destroy(p);
    msg_destroy(decoded);
    return 0;
}

/* ================================================================
 * 边界条件
 * ================================================================ */
MU_TEST(test_null_handling) {
    mu_assert_int_eq(MSG_ERR_NULL_PTR, msg_set_func(NULL, "test"));
    mu_assert_int_eq(MSG_ERR_NULL_PTR, msg_set_headers(NULL, 0, "a"));
    mu_assert_int_eq(MSG_ERR_NULL_PTR, msg_add_row(NULL));
    mu_assert_int_eq(MSG_ERR_NULL_PTR, msg_finalize(NULL));
    mu_assert_int_eq(MSG_ERR_NULL_PTR, msg_encode(NULL, NULL, NULL));
    mu_check(msg_data(NULL) == NULL);
    mu_assert_int_eq(0, msg_size(NULL));
    return 0;
}

MU_TEST(test_invalid_magic) {
    uint8_t bad_packet[128] = {0};
    memcpy(bad_packet, "XXXX", 4);
    memset(bad_packet + 4, 0, 72);
    bad_packet[4] = 0x00; bad_packet[5] = 0x00;
    bad_packet[6] = 0x00; bad_packet[7] = 0x10;

    msg_packet_t *result = NULL;
    int ret = msg_decode(bad_packet, sizeof(bad_packet), &result);
    mu_assert_int_eq(MSG_ERR_INVALID_MAGIC, ret);
    mu_check(result == NULL);
    return 0;
}

MU_TEST(test_oversized_body) {
    uint8_t fake[128] = {0};
    memcpy(fake, MSG_MAGIC, 4);
    fake[4] = 0x00; fake[5] = 0x00; fake[6] = 0x00; fake[7] = 0x80;

    msg_packet_t *result = NULL;
    int ret = msg_decode(fake, sizeof(fake), &result);
    mu_check(ret != 0);
    mu_check(result == NULL);
    return 0;
}

/* ================================================================
 * msg_clone 直接测试
 * ================================================================ */
MU_TEST(test_msg_clone_direct) {
    /* 构建源包 */
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    msg_set_func(p, "clone");
    msg_set_headers(p, 3, "A,B,C");
    msg_add_row(p);
    msg_set_value_str(p, "A", "val1");
    msg_set_value_str(p, "B", "val2");
    msg_set_value_str(p, "C", "val3");
    mu_assert_int_eq(0, msg_finalize(p));

    /* 克隆 */
    msg_packet_t *c = msg_clone(p);
    mu_check(c != NULL);
    mu_assert_str_eq(msg_get_func(p), msg_get_func(c));
    mu_check(strcmp(msg_get_msg_id(p), msg_get_msg_id(c)) == 0);

    /* 克隆后 decode（使其进入解析状态可修改），再验证内容 */
    void *buf = NULL;
    size_t buf_len = 0;
    mu_assert_int_eq(0, msg_encode(c, &buf, &buf_len));
    msg_destroy(c);

    msg_packet_t *d = NULL;
    mu_assert_int_eq(0, msg_decode(buf, buf_len, &d));
    mu_check(d != NULL);
    mu_assert_str_eq("val1", get_row_value_by_key(d, "A"));
    mu_assert_str_eq("val2", get_row_value_by_key(d, "B"));
    mu_assert_str_eq("val3", get_row_value_by_key(d, "C"));

    /* 解码后可以添加新行（但不可再次 finalize，克隆的包已 finalized） */
    mu_assert_int_eq(0, msg_add_row(d));
    mu_assert_int_eq(0, msg_set_value_str(d, "A", "new_val1"));
    mu_assert_int_eq(2, msg_get_row_count(d));

    msg_free_buffer(buf);
    msg_destroy(p);
    msg_destroy(d);
    return 0;
}

/* ================================================================
 * 游标遍历：msg_fetch_next / msg_reset_cursor / msg_get_current_row
 * ================================================================ */
MU_TEST(test_msg_fetch_next_cursor) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    msg_set_headers(p, 2, "K,V");
    msg_add_row(p);
    msg_set_value_str(p, "K", "key1");
    msg_set_value_str(p, "V", "val1");
    msg_add_row(p);
    msg_set_value_str(p, "K", "key2");
    msg_set_value_str(p, "V", "val2");
    msg_add_row(p);
    msg_set_value_str(p, "K", "key3");
    msg_set_value_str(p, "V", "val3");

    mu_assert_int_eq(0, msg_finalize(p));

    /* 先调用 fetch_next 让游标生效 */
    mu_check(msg_fetch_next(p));
    mu_assert_int_eq(0, msg_get_current_row(p));

    /* 遍历 */
    mu_check(msg_fetch_next(p));
    mu_assert_int_eq(1, msg_get_current_row(p));

    mu_check(msg_fetch_next(p));
    mu_assert_int_eq(2, msg_get_current_row(p));

    /* 已到最后，无更多行 */
    mu_check(!msg_fetch_next(p));

    /* 重置后从头开始 */
    msg_reset_cursor(p);
    mu_check(msg_fetch_next(p));
    mu_assert_int_eq(0, msg_get_current_row(p));

    msg_destroy(p);
    return 0;
}

/* ================================================================
 * 多结果集：msg_add_result_set / msg_next_result_set / msg_select_result_set
 * ================================================================ */
MU_TEST(test_msg_multi_result_set) {
    msg_packet_t *p = msg_create(MSG_TYPE_ANSWER, "V1.0");
    mu_check(p != NULL);

    /* RS1 */
    msg_set_headers(p, 2, "Sym,Price");
    msg_add_row(p);
    msg_set_value_str(p, "Sym", "BTC");
    msg_set_value_str(p, "Price", "50000");

    /* 添加 RS2 */
    mu_check(msg_add_result_set(p));
    mu_assert_int_eq(2, msg_get_result_set(p));
    msg_set_headers(p, 2, "Tag,Note");
    msg_add_row(p);
    msg_set_value_str(p, "Tag", "test");
    msg_set_value_str(p, "Note", "note1");

    /* 添加 RS3 */
    mu_check(msg_add_result_set(p));
    mu_assert_int_eq(3, msg_get_result_set(p));

    mu_assert_int_eq(0, msg_finalize(p));

    /* 编码解码后验证 */
    void *buf = NULL;
    size_t buf_len = 0;
    mu_assert_int_eq(0, msg_encode(p, &buf, &buf_len));
    msg_destroy(p);

    msg_packet_t *d = NULL;
    mu_assert_int_eq(0, msg_decode(buf, buf_len, &d));
    mu_check(d != NULL);
    mu_assert_int_eq(3, msg_get_result_set_count(d));

    /* 选择 RS1 */
    mu_assert_int_eq(0, msg_select_result_set(d, 1));
    mu_assert_str_eq("BTC", get_row_value_by_key(d, "Sym"));

    /* 选择 RS2 */
    mu_assert_int_eq(0, msg_select_result_set(d, 2));
    mu_assert_str_eq("test", get_row_value_by_key(d, "Tag"));

    /* 选择 RS3 */
    mu_assert_int_eq(0, msg_select_result_set(d, 3));
    /* 验证在 RS3 中 */
    mu_assert_int_eq(3, msg_get_result_set(d));

    msg_free_buffer(buf);
    msg_destroy(d);
    return 0;
}

/* ================================================================
 * 字符串值获取：msg_get_value_str
 * ================================================================ */
MU_TEST(test_msg_get_value_str) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    msg_set_headers(p, 2, "K,V");
    msg_add_row(p);
    msg_set_value_str(p, "K", "");
    msg_set_value_str(p, "V", "hello");

    mu_assert_int_eq(0, msg_finalize(p));

    /* finalize 后需要编码再解码才能调用 msg_get_value_*（需要 unescaped_body） */
    void *buf = NULL;
    size_t buf_len = 0;
    mu_assert_int_eq(0, msg_encode(p, &buf, &buf_len));
    msg_destroy(p);

    msg_packet_t *d = NULL;
    mu_assert_int_eq(0, msg_decode(buf, buf_len, &d));

    /* 解码后需要先 fetch_next 设置游标 */
    mu_check(msg_fetch_next(d));

    const char *val = NULL;
    size_t len = 0;

    /* 空字符串 */
    mu_assert_int_eq(0, msg_get_value_str(d, "K", &val, &len));
    mu_check(val != NULL);
    mu_assert_int_eq(0, len);

    /* 正常字符串 */
    mu_assert_int_eq(0, msg_get_value_str(d, "V", &val, &len));
    mu_assert_int_eq(5, len);
    mu_assert_mem_eq("hello", val, 5);

    /* 大小写不敏感 */
    mu_assert_int_eq(0, msg_get_value_str(d, "k", &val, &len));

    msg_free_buffer(buf);
    msg_destroy(d);
    return 0;
}

/* ================================================================
 * 按行列索引读取：msg_get_field
 * ================================================================ */
MU_TEST(test_msg_get_field_by_index) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    msg_set_headers(p, 2, "A,B");
    msg_add_row(p);
    msg_set_value_str(p, "A", "row0col0");
    msg_set_value_str(p, "B", "row0col1");
    msg_add_row(p);
    msg_set_value_str(p, "A", "row1col0");
    msg_set_value_str(p, "B", "row1col1");

    mu_assert_int_eq(0, msg_finalize(p));

    /* msg_get_field 需要 unescaped_body（decode 后才有），所以先编码再解码 */
    void *buf = NULL;
    size_t buf_len = 0;
    mu_assert_int_eq(0, msg_encode(p, &buf, &buf_len));
    msg_destroy(p);

    msg_packet_t *d = NULL;
    mu_assert_int_eq(0, msg_decode(buf, buf_len, &d));

    const char *val = NULL;
    size_t len = 0;

    mu_assert_int_eq(0, msg_get_field(d, 0, 0, &val, &len));
    mu_assert_int_eq(8, len);
    mu_assert_mem_eq("row0col0", val, 8);

    mu_assert_int_eq(0, msg_get_field(d, 1, 1, &val, &len));
    mu_assert_int_eq(8, len);
    mu_assert_mem_eq("row1col1", val, 8);

    /* 超范围 */
    mu_assert_int_eq(MSG_ERR_NO_DATA, msg_get_field(d, 10, 0, &val, &len));

    msg_free_buffer(buf);
    msg_destroy(d);
    return 0;
}

/* ================================================================
 * msg_wire_to_string：wire 可读化
 * ================================================================ */
MU_TEST(test_msg_wire_to_string) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    msg_set_func(p, "test");
    msg_set_headers(p, 2, "K,V");
    msg_add_row(p);
    msg_set_value_str(p, "K", "key");
    msg_set_value_str(p, "V", "val");

    mu_assert_int_eq(0, msg_finalize(p));

    char *str = msg_wire_to_string(p);
    mu_check(str != NULL);
    /* 应包含 msg_id、功能名、表头等可读内容 */
    mu_check(strlen(str) > 0);
    /* 验证转义符号替代 */
    /* 实际上 wire 中 US 会被显示为可读标记 */
    free(str);

    msg_destroy(p);
    return 0;
}

/* ================================================================
 * msg_get_headers 重建表头字符串
 * ================================================================ */
MU_TEST(test_msg_get_headers_api) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    msg_set_headers(p, 3, "Col1,Col2,Col3");

    char buf[128];
    size_t len = sizeof(buf);
    mu_assert_int_eq(0, msg_get_headers(p, buf, &len));
    mu_assert_str_eq("Col1,Col2,Col3", buf);

    /* 缓冲区过小 */
    char small[8];
    size_t small_len = sizeof(small);
    mu_assert_int_eq(MSG_ERR_BUFFER_TOO_SMALL, msg_get_headers(p, small, &small_len));

    msg_destroy(p);
    return 0;
}

/* ================================================================
 * MSG_TYPE_PUSH / MSG_TYPE_HEARTBEAT 构建和解码
 * ================================================================ */
MU_TEST(test_msg_type_push_heartbeat) {
    /* PUSH 包 */
    msg_packet_t *push = msg_create(MSG_TYPE_PUSH, "V1.0");
    mu_check(push != NULL);
    mu_assert_int_eq(MSG_TYPE_PUSH, msg_get_type(push));
    msg_set_func(push, "notify");
    msg_set_headers(push, 1, "Status");
    msg_add_row(push);
    msg_set_value_str(push, "Status", "online");
    mu_assert_int_eq(0, msg_finalize(push));

    void *buf = NULL;
    size_t buf_len = 0;
    mu_assert_int_eq(0, msg_encode(push, &buf, &buf_len));
    msg_destroy(push);

    msg_packet_t *d = NULL;
    mu_assert_int_eq(0, msg_decode(buf, buf_len, &d));
    mu_assert_int_eq(MSG_TYPE_PUSH, msg_get_type(d));
    mu_assert_str_eq("notify", msg_get_func(d));
    msg_free_buffer(buf);
    msg_destroy(d);

    /* HEARTBEAT 包 */
    msg_packet_t *hb = msg_create(MSG_TYPE_HEARTBEAT, "V1.0");
    mu_check(hb != NULL);
    mu_assert_int_eq(MSG_TYPE_HEARTBEAT, msg_get_type(hb));
    mu_assert_int_eq(0, msg_finalize(hb));

    mu_assert_int_eq(0, msg_encode(hb, &buf, &buf_len));
    msg_destroy(hb);

    mu_assert_int_eq(0, msg_decode(buf, buf_len, &d));
    mu_assert_int_eq(MSG_TYPE_HEARTBEAT, msg_get_type(d));
    msg_free_buffer(buf);
    msg_destroy(d);

    return 0;
}

/* ================================================================
 * 数值类型读取：msg_get_value_i32/i64/double
 * ================================================================ */
MU_TEST(test_msg_numeric_getters) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);
    msg_set_headers(p, 4, "I32,I64,Dbl,Str");
    msg_add_row(p);
    msg_set_value_i32(p, "I32", -12345);
    msg_set_value_i64(p, "I64", 9223372036854775807LL);
    msg_set_value_double(p, "Dbl", 2.718281828);
    msg_set_value_str(p, "Str", "not_a_number");

    mu_assert_int_eq(0, msg_finalize(p));

    /* 需要先编码再解码才能使用 msg_get_value_*（需要 unescaped_body） */
    void *buf = NULL;
    size_t buf_len = 0;
    mu_assert_int_eq(0, msg_encode(p, &buf, &buf_len));
    msg_destroy(p);

    msg_packet_t *d = NULL;
    mu_assert_int_eq(0, msg_decode(buf, buf_len, &d));

    /* 解码后需要先 fetch_next 设置游标 */
    mu_check(msg_fetch_next(d));

    int32_t i32 = 0;
    mu_assert_int_eq(0, msg_get_value_i32(d, "I32", &i32));
    mu_assert_int_eq(-12345, i32);

    int64_t i64 = 0;
    mu_assert_int_eq(0, msg_get_value_i64(d, "I64", &i64));
    mu_assert_int_eq(9223372036854775807LL, i64);

    double dbl = 0.0;
    mu_assert_int_eq(0, msg_get_value_double(d, "Dbl", &dbl));
    mu_assert_double_eq(2.718281828, dbl, 0.000001);

    /* 注意：atof("not_a_number") 返回 0.0，无法区分无效输入和真正的 0 值 */
    mu_assert_int_eq(0, msg_get_value_double(d, "Str", &dbl));
    mu_assert_double_eq(0.0, dbl, 0.000001);

    msg_free_buffer(buf);
    msg_destroy(d);
    return 0;
}

/* ================================================================
 * 边界测试：too many headers / too many rows
 * ================================================================ */
MU_TEST(test_msg_boundary_oversized) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    mu_check(p != NULL);

    /* 过量表头（超过 MSG_MAX_HEADERS=256） */
    char many_hdrs[1024] = {0};
    for (int i = 0; i < 257; i++) {
        if (i > 0) many_hdrs[i - 1] = ',';
        many_hdrs[i] = 'H';
    }
    int ret = msg_set_headers(p, 257, many_hdrs);
    mu_assert_int_eq(MSG_ERR_TOO_MANY_HEADERS, ret);

    msg_destroy(p);
    return 0;
}

/* ================================================================
 * UUID 生成
 * ================================================================ */
MU_TEST(test_uuid_v4_format) {
    char uuid[33];
    msg_generate_uuid_v4(uuid);
    uuid[32] = '\0';

    mu_assert_int_eq(32, strlen(uuid));

    for (int i = 0; i < 32; i++) {
        mu_check(isxdigit((unsigned char)uuid[i]));
    }

    mu_assert_int_eq('4', uuid[12]);

    mu_check(uuid[16] == '8' || uuid[16] == '9' ||
             uuid[16] == 'A' || uuid[16] == 'B');
    return 0;
}

/* ================================================================
 * 简化性能测试
 * ================================================================ */
#include <time.h>

static uint64_t test_get_time_ms(void) {
    clock_t c = clock();
    return (uint64_t)((double)c / CLOCKS_PER_SEC * 1000.0);
}

MU_TEST(test_perf_basic) {
    uint64_t start = test_get_time_ms();
    for (int i = 0; i < 1000; i++) {
        msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
        msg_destroy(p);
    }
    uint64_t elapsed = test_get_time_ms() - start;
    printf("\n  [perf] create_destroy: %llu ms for 1000 iterations", (unsigned long long)elapsed);
    mu_check(elapsed < 5000);
    return 0;
}

/* ================================================================
 * 测试套件
 * ================================================================ */
MU_TEST_SUITE(all_tests) {
    MU_RUN_TEST(test_crc32_known_vectors);
    MU_RUN_TEST(test_escape_unescape_cycle);
    MU_RUN_TEST(test_msg_create_destroy);
    MU_RUN_TEST(test_msg_clone_direct);
    MU_RUN_TEST(test_msg_fetch_next_cursor);
    MU_RUN_TEST(test_msg_multi_result_set);
    MU_RUN_TEST(test_msg_get_value_str);
    MU_RUN_TEST(test_msg_get_field_by_index);
    MU_RUN_TEST(test_msg_wire_to_string);
    MU_RUN_TEST(test_msg_get_headers_api);
    MU_RUN_TEST(test_msg_type_push_heartbeat);
    MU_RUN_TEST(test_msg_numeric_getters);
    MU_RUN_TEST(test_msg_boundary_oversized);
    MU_RUN_TEST(test_msg_clone_no_leak);
    MU_RUN_TEST(test_msg_header_fields);
    MU_RUN_TEST(test_msg_headers_and_rows);
    MU_RUN_TEST(test_msg_numeric_values);
    MU_RUN_TEST(test_full_encode_decode_cycle);
    MU_RUN_TEST(test_null_handling);
    MU_RUN_TEST(test_invalid_magic);
    MU_RUN_TEST(test_oversized_body);
    MU_RUN_TEST(test_uuid_v4_format);
    MU_RUN_TEST(test_perf_basic);
}

/* ================================================================
 * main
 * ================================================================ */
int main(void) {
    printf("=== MsgPacket Unit Tests (MinUnit) ===\n\n");
    MU_RUN_SUITE(all_tests);
    MU_PRINT_REPORT();
    return mu_failed > 0 ? 1 : 0;
}
