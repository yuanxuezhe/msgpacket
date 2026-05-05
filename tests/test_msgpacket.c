/*
 * test_msgpacket.c — MsgPacket 单元测试
 * 测试覆盖：创建/销毁、克隆、CRC32、封包解包、错误处理
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "msg_api.h"
#include "msg_util.h"
#include "msg_packet.h"
#include "msg_byteorder.h"

/* ================================================================
 * 辅助函数（使用 public API，避免内部类型）
 * ================================================================ */

/* 获取当前 result set 中指定 key 的值
 * 通过 msg_get_headers 获取 header 列表，找到 key 对应的列索引，再取值
 * 注意：返回的指针在 packet 销毁前有效，但字符串长度由 *out_len 指定 */
static const char *get_row_value_by_key(msg_packet_t *p, const char *key) {
    size_t col_count = msg_get_header_count(p);
    if (col_count == 0) return NULL;

    /* 获取完整 header 字符串，用逗号分割找到列索引 */
    char headers[256];
    size_t hdr_buf_len = sizeof(headers);
    if (msg_get_headers(p, headers, &hdr_buf_len) != 0) return NULL;

    /* 在 headers 中找 key 的位置（逗号分隔） */
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

    /* 用 msg_get_value_str 获取值（它会正确处理 cursor_row）
     * 注意：返回的 val 没有 null terminator，需要用 val_len 限制长度
     * 返回 static buffer 以便调用者使用 */
    static char val_buf[256];
    const char *val = NULL;
    size_t val_len = 0;
    if (msg_get_value_str(p, key, &val, &val_len) != 0) return NULL;
    if (val_len >= sizeof(val_buf)) val_len = sizeof(val_buf) - 1;
    memcpy(val_buf, val, val_len);
    val_buf[val_len] = '\0';
    return val_buf;
}

    /* 获取解码后 packet 第一行指定 key 的值 */
static const char *get_decoded_row_value(msg_packet_t *p, const char *key) {
    return get_row_value_by_key(p, key);
}

/* ================================================================
 * CRC32 标准向量验证
 * ================================================================ */

static void test_crc32_known_vectors(void **state) {
    (void)state;

    crc32_init();

    /* RFC 3720 Appendix B 标准测试向量 */
    struct {
        const uint8_t *data;
        size_t len;
        uint32_t expected;
    } vectors[] = {
        /* 空输入 */
        {(uint8_t*)"", 0, 0x00000000},
        /* 全 0 字节（标准 CRC32 / zlib / 以太网校验和） */
        {(uint8_t*)"\x00\x00\x00\x00", 4, 0x2144DF1C},
        /* 全 0xFF 字节 */
        {(uint8_t*)"\xFF\xFF\xFF\xFF", 4, 0xFFFFFFFF},
        /* "123456789" — 最常用校验向量 */
        {(uint8_t*)"123456789", 9, 0xCBF43926},
    };

    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        uint32_t crc = crc32_update(0, vectors[i].data, vectors[i].len);
        assert_int_equal(vectors[i].expected, crc);
    }
}

/* ================================================================
 * 转义/反转义
 * ================================================================ */

static void test_escape_unescape_cycle(void **state) {
    (void)state;

    /* 普通数据 */
    uint8_t raw[] = "hello world";
    size_t esc_len, unesc_len;
    uint8_t *esc = msg_escape(raw, sizeof(raw) - 1, &esc_len);
    assert_non_null(esc);
    uint8_t *unesc = msg_unescape(esc, esc_len, &unesc_len);
    assert_non_null(unesc);
    assert_int_equal(sizeof(raw) - 1, unesc_len);
    assert_memory_equal(raw, unesc, sizeof(raw) - 1);
    free(esc);
    free(unesc);

    /* 含转义字符数据（Unit Separator 0x1F） */
    uint8_t raw2[] = "a\x1Fb\x1Ec\x1Cd";
    uint8_t *esc2 = msg_escape(raw2, sizeof(raw2) - 1, &esc_len);
    uint8_t *unesc2 = msg_unescape(esc2, esc_len, &unesc_len);
    assert_non_null(unesc2);
    assert_int_equal(sizeof(raw2) - 1, unesc_len);
    assert_memory_equal(raw2, unesc2, sizeof(raw2) - 1);
    free(esc2);
    free(unesc2);

    /* 含 ESC 本身（0x1B）后跟普通字符 */
    uint8_t raw3[] = "a\x1Bb";
    uint8_t *esc3 = msg_escape(raw3, sizeof(raw3) - 1, &esc_len);
    uint8_t *unesc3 = msg_unescape(esc3, esc_len, &unesc_len);
    assert_non_null(unesc3);
    assert_memory_equal(raw3, unesc3, sizeof(raw3) - 1);
    free(esc3);
    free(unesc3);

    /* 孤立 ESC → 必须返回 NULL */
    uint8_t bad[] = "\x1B";
    size_t out_len;
    assert_null(msg_unescape(bad, 1, &out_len));

    /* 无效转义后缀 */
    uint8_t bad2[] = "\x1BX";
    assert_null(msg_unescape(bad2, 2, &out_len));
}

/* ================================================================
 * 创建 / 销毁
 * ================================================================ */

static void test_msg_create_destroy(void **state) {
    (void)state;

    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    assert_non_null(p);
    assert_int_equal(MSG_TYPE_REQUEST, msg_get_type(p));
    assert_string_equal("V1.0", msg_get_version(p));
    assert_non_null(msg_get_msg_id(p));
    assert_int_equal(32, strlen(msg_get_msg_id(p)));
    msg_destroy(p);

    /* NULL 销毁不崩溃 */
    msg_destroy(NULL);
}

/* ================================================================
 * msg_clone 内存泄漏修复验证
 * ================================================================ */

static void test_msg_clone_no_leak(void **state) {
    (void)state;

    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    assert_non_null(p);

    int ret = msg_set_headers(p, 2, "Name,Age");
    assert_int_equal(0, ret);

    ret = msg_add_row(p);
    assert_int_equal(0, ret);

    ret = msg_set_value_str(p, "Name", "Alice");
    assert_int_equal(0, ret);

    ret = msg_set_value_str(p, "Age", "30");
    assert_int_equal(0, ret);

    assert_int_equal(0, msg_finalize(p));

    /* 序列化再反序列化，确保 unescaped_body 有数据 */
    void *buf = NULL;
    size_t buf_len = 0;
    ret = msg_encode(p, &buf, &buf_len);
    assert_int_equal(0, ret);
    assert_non_null(buf);
    assert_true(buf_len > 0);

    msg_packet_t *decoded = NULL;
    ret = msg_decode(buf, buf_len, &decoded);
    assert_int_equal(0, ret);
    assert_non_null(decoded);

    /* 从已解码的包克隆（unescaped_body 已复制） */
    msg_packet_t *c = msg_clone(decoded);
    assert_non_null(c);
    assert_string_equal(msg_get_msg_id(decoded), msg_get_msg_id(c));

    /* 验证克隆内容正确 */
    assert_string_equal("Alice", get_row_value_by_key(c, "Name"));
    assert_string_equal("30", get_row_value_by_key(c, "Age"));

    /* 销毁原包，克隆应不受影响 */
    msg_destroy(decoded);

    /* 验证克隆内容还在 */
    assert_string_equal("Alice", get_row_value_by_key(c, "Name"));

    msg_destroy(c);
    msg_destroy(p);
    msg_free_buffer(buf);
}

/* ================================================================
 * Header 字段设置 / 获取
 * ================================================================ */

static void test_msg_header_fields(void **state) {
    (void)state;

    msg_packet_t *p = msg_create(MSG_TYPE_ANSWER, "V1.0");
    assert_non_null(p);

    assert_int_equal(0, msg_set_func(p, "getUsr"));
    assert_string_equal("getUsr", msg_get_func(p));

    assert_int_equal(0, msg_set_timestamp(p, "20250505000000000"));
    assert_string_equal("20250505000000000", msg_get_timestamp(p));

    assert_int_equal(0, msg_set_format(p, MSG_FORMAT_TABLE));
    assert_int_equal(MSG_FORMAT_TABLE, msg_get_format(p));

    msg_destroy(p);
}

/* ================================================================
 * 表头 / 数据行
 * ================================================================ */

static void test_msg_headers_and_rows(void **state) {
    (void)state;

    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    assert_non_null(p);

    /* 设置表头 */
    assert_int_equal(0, msg_set_headers(p, 3, "Symbol,Price,Volume"));

    /* 获取表头 */
    char buf[256];
    size_t len = sizeof(buf);
    assert_int_equal(0, msg_get_headers(p, buf, &len));
    assert_string_equal("Symbol,Price,Volume", buf);

    /* 添加行 */
    assert_int_equal(0, msg_add_row(p));
    assert_int_equal(0, msg_set_value_str(p, "Symbol", "BTCUSD"));
    assert_int_equal(0, msg_set_value_str(p, "Price", "50000"));
    assert_int_equal(0, msg_set_value_str(p, "Volume", "100"));

    /* 再添加一行 */
    assert_int_equal(0, msg_add_row(p));
    assert_int_equal(0, msg_set_value_str(p, "Symbol", "ETHUSD"));
    assert_int_equal(0, msg_set_value_str(p, "Price", "3000"));
    assert_int_equal(0, msg_set_value_str(p, "Volume", "500"));

    /* 清除所有行 */
    assert_int_equal(0, msg_clear_rows(p));

    msg_destroy(p);
}

static void test_msg_numeric_values(void **state) {
    (void)state;

    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    assert_non_null(p);
    assert_int_equal(0, msg_set_headers(p, 4, "Id,I64,Dbl,Str"));
    assert_int_equal(0, msg_add_row(p));
    assert_int_equal(0, msg_set_value_i32(p, "Id", 12345));
    assert_int_equal(0, msg_set_value_i64(p, "I64", -12345678901234LL));
    assert_int_equal(0, msg_set_value_double(p, "Dbl", 3.14159));
    assert_int_equal(0, msg_set_value_str(p, "Str", "test"));

    /* finalize 后验证 wire 数据 */
    assert_int_equal(0, msg_finalize(p));
    assert_non_null(msg_data(p));
    assert_true(msg_size(p) > 0);

    msg_destroy(p);
}

/* ================================================================
 * 封包 / 解包 完整流程
 * ================================================================ */

static void test_full_encode_decode_cycle(void **state) {
    (void)state;

    /* 构建方 */
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    assert_non_null(p);
    msg_set_func(p, "query");
    msg_set_headers(p, 2, "Key,Val");
    msg_add_row(p);
    msg_set_value_str(p, "Key", "hostname");
    msg_set_value_str(p, "Val", "server1");
    assert_int_equal(0, msg_finalize(p));

    /* 编码为独立缓冲区 */
    void *buf = NULL;
    size_t buf_len = 0;
    assert_int_equal(0, msg_encode(p, &buf, &buf_len));
    assert_non_null(buf);
    assert_true(buf_len > 0);

    /* 解码方 */
    msg_packet_t *decoded = NULL;
    assert_int_equal(0, msg_decode(buf, buf_len, &decoded));
    assert_non_null(decoded);

    /* 验证 header 字段 */
    assert_int_equal(MSG_TYPE_REQUEST, msg_get_type(decoded));
    assert_string_equal("query", msg_get_func(decoded));
    assert_string_equal("V1.0", msg_get_version(decoded));

    /* 验证 header 列表 */
    char hdr_buf[64];
    size_t hdr_len = sizeof(hdr_buf);
    assert_int_equal(0, msg_get_headers(decoded, hdr_buf, &hdr_len));
    assert_string_equal("Key,Val", hdr_buf);

    /* 验证数据行内容 */
    assert_string_equal("hostname", get_row_value_by_key(decoded, "Key"));
    assert_string_equal("server1", get_row_value_by_key(decoded, "Val"));

    /* 清理 */
    msg_free_buffer(buf);
    msg_destroy(p);
    msg_destroy(decoded);
}

/* ================================================================
 * 边界条件
 * ================================================================ */

static void test_null_handling(void **state) {
    (void)state;

    /* 所有接受 packet 的 API 对 NULL 输入应返回错误码 */
    assert_int_equal(MSG_ERR_NULL_PTR, msg_set_func(NULL, "test"));
    assert_int_equal(MSG_ERR_NULL_PTR, msg_set_headers(NULL, 0, "a"));
    assert_int_equal(MSG_ERR_NULL_PTR, msg_add_row(NULL));
    assert_int_equal(MSG_ERR_NULL_PTR, msg_finalize(NULL));
    assert_int_equal(MSG_ERR_NULL_PTR, msg_encode(NULL, NULL, NULL));
    assert_null(msg_data(NULL));
    assert_int_equal(0, msg_size(NULL));
}

static void test_invalid_magic(void **state) {
    (void)state;

    /* 伪造错误 magic：完整的 header 但 magic 不是 MSGPACK */
    uint8_t bad_packet[128] = {0};
    memcpy(bad_packet, "XXXX", 4);  /* 错误 magic */
    /* header 大小 + 足够 body */
    memset(bad_packet + 4, 0, 72);  /* 其余 header 字段全 0 */
    bad_packet[4] = 0x00; bad_packet[5] = 0x00;
    bad_packet[6] = 0x00; bad_packet[7] = 0x10;  /* body_len = 16 */

    msg_packet_t *result = NULL;
    int ret = msg_decode(bad_packet, sizeof(bad_packet), &result);
    assert_int_equal(MSG_ERR_INVALID_MAGIC, ret);
    assert_null(result);
}

static void test_oversized_body(void **state) {
    (void)state;

    /* 构建超大 body_len 包（模拟恶意数据） */
    uint8_t fake[128] = {0};
    memcpy(fake, MSG_MAGIC, 4);
    /* body_len = 0x80000000（超大值）*/
    fake[4] = 0x00; fake[5] = 0x00; fake[6] = 0x00; fake[7] = 0x80;

    msg_packet_t *result = NULL;
    int ret = msg_decode(fake, sizeof(fake), &result);
    assert_true(ret != 0);  /* 应返回错误码 */
    assert_null(result);
}

/* ================================================================
 * UUID 生成
 * ================================================================ */

static void test_uuid_v4_format(void **state) {
    (void)state;

    char uuid[33];
    msg_generate_uuid_v4(uuid);
    uuid[32] = '\0';

    /* 长度 32 */
    assert_int_equal(32, strlen(uuid));

    /* 字符全是十六进制 */
    for (int i = 0; i < 32; i++) {
        assert_true(isxdigit((unsigned char)uuid[i]));
    }

    /* 版本 4：第 13 位必须是 '4' */
    assert_int_equal('4', uuid[12]);

    /* 变体 8/9/a/b：第 17 位必须是 8/9/A/B */
    assert_true(uuid[16] == '8' || uuid[16] == '9' ||
                uuid[16] == 'A' || uuid[16] == 'B');
}

/* ================================================================
 * main
 * ================================================================ */

int main(void) {
    const struct CMUnitTest tests[] = {
        /* CRC32 */
        cmocka_unit_test(test_crc32_known_vectors),
        /* 转义 */
        cmocka_unit_test(test_escape_unescape_cycle),
        /* 创建/销毁 */
        cmocka_unit_test(test_msg_create_destroy),
        /* 克隆内存泄漏修复 */
        cmocka_unit_test(test_msg_clone_no_leak),
        /* Header 字段 */
        cmocka_unit_test(test_msg_header_fields),
        /* 表头/数据行 */
        cmocka_unit_test(test_msg_headers_and_rows),
        cmocka_unit_test(test_msg_numeric_values),
        /* 完整流程 */
        cmocka_unit_test(test_full_encode_decode_cycle),
        /* 边界条件 */
        cmocka_unit_test(test_null_handling),
        cmocka_unit_test(test_invalid_magic),
        cmocka_unit_test(test_oversized_body),
        /* UUID */
        cmocka_unit_test(test_uuid_v4_format),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
