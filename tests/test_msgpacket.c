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
 * 测试套件
 * ================================================================ */
MU_TEST_SUITE(all_tests) {
    MU_RUN_TEST(test_crc32_known_vectors);
    MU_RUN_TEST(test_escape_unescape_cycle);
    MU_RUN_TEST(test_msg_create_destroy);
    MU_RUN_TEST(test_msg_clone_no_leak);
    MU_RUN_TEST(test_msg_header_fields);
    MU_RUN_TEST(test_msg_headers_and_rows);
    MU_RUN_TEST(test_msg_numeric_values);
    MU_RUN_TEST(test_full_encode_decode_cycle);
    MU_RUN_TEST(test_null_handling);
    MU_RUN_TEST(test_invalid_magic);
    MU_RUN_TEST(test_oversized_body);
    MU_RUN_TEST(test_uuid_v4_format);
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
