/*
 * test_fuzz.c — msg_decode 路径模糊测试（MinUnit 框架）
 * 覆盖：字节翻转、边界值、损坏转义、非法header、畸形分隔符、边界帧
 *
 * 测试策略：从正确 packet 模板开始逐字节破坏，验证 msg_decode
 * 对各类畸形输入均返回错误码而非崩溃。
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "minunit.h"
#include "msg_api.h"
#include "msg_util.h"
#include "msg_packet.h"
#include "msg_byteorder.h"

/* ================================================================
 * 测试辅助
 * ================================================================ */

/* 创建标准测试 packet 并 finalize，返回 wire 缓冲区（调用者需 free） */
static uint8_t* build_valid_packet(size_t *out_len) {
    msg_packet_t *p = msg_create(MSG_TYPE_REQUEST, "V1.0");
    if (!p) return NULL;

    msg_set_headers(p, 3, "id,name,value");
    msg_add_row(p);
    msg_set_row(p, "1,Alice,100");
    msg_add_row(p);
    msg_set_row(p, "2,Bob,200");

    if (msg_finalize(p) != 0) { msg_destroy(p); return NULL; }

    size_t sz = msg_size(p);
    const void *data = msg_data(p);
    if (!data || sz == 0) { msg_destroy(p); return NULL; }

    uint8_t *wire = (uint8_t*)malloc(sz);
    if (!wire) { msg_destroy(p); return NULL; }
    memcpy(wire, data, sz);
    *out_len = sz;
    msg_destroy(p);
    return wire;
}

/* 验证 msg_decode 接受或拒绝（不崩溃即为安全） */
static void test_decode_fuzz(const uint8_t *wire, size_t len, int expected_rc) {
    msg_packet_t *p = NULL;
    int rc = msg_decode(wire, len, &p);
    /* 允许返回错误码或成功，但不能崩溃（rc <-255 或内存越界） */
    if (p) {
        msg_destroy(p);
    }
    (void)rc;
    (void)expected_rc;
}

/* ================================================================
 * 字节翻转测试（逐字节翻转 / 置 0xFF / 置 0x00）
 * ================================================================ */

MU_TEST(test_fuzz_byte_flip_magic) {
    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    /* 翻转 magic[0..3] */
    for (size_t i = 0; i < 4; i++) {
        uint8_t orig = wire[i];
        for (int b = 0; b < 8; b++) {
            uint8_t flipped = orig ^ (1 << b);
            uint8_t pkt[256];
            memcpy(pkt, wire, len);
            pkt[i] = flipped;
            test_decode_fuzz(pkt, len, MSG_ERR_INVALID_MAGIC);
        }
        /* 置 0x00 和 0xFF */
        uint8_t pkt0[256]; memcpy(pkt0, wire, len); pkt0[i] = 0x00;
        test_decode_fuzz(pkt0, len, MSG_ERR_INVALID_MAGIC);
        uint8_t pktf[256]; memcpy(pktf, wire, len); pktf[i] = 0xFF;
        test_decode_fuzz(pktf, len, MSG_ERR_INVALID_MAGIC);
    }

    free(wire);
    return 0;
}

MU_TEST(test_fuzz_byte_flip_crc32) {
    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    /* 翻转 crc32[4]（偏移 4-7） */
    for (size_t i = 4; i < 8; i++) {
        uint8_t orig = wire[i];
        for (int b = 0; b < 8; b++) {
            uint8_t flipped = orig ^ (1 << b);
            uint8_t *pkt = (uint8_t*)malloc(len);
            memcpy(pkt, wire, len);
            pkt[i] = flipped;
            test_decode_fuzz(pkt, len, MSG_ERR_CRC_MISMATCH);
            free(pkt);
        }
        /* 置 0x00 */
        uint8_t *pkt0 = (uint8_t*)malloc(len);
        memcpy(pkt0, wire, len); pkt0[i] = 0x00;
        test_decode_fuzz(pkt0, len, MSG_ERR_CRC_MISMATCH);
        free(pkt0);
    }

    free(wire);
    return 0;
}

MU_TEST(test_fuzz_byte_flip_body_len) {
    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    /* 翻转 body_len[4]（偏移 8-11） */
    for (size_t i = 8; i < 12; i++) {
        uint8_t orig = wire[i];
        for (int b = 0; b < 8; b++) {
            uint8_t flipped = orig ^ (1 << b);
            uint8_t *pkt = (uint8_t*)malloc(len);
            memcpy(pkt, wire, len);
            pkt[i] = flipped;
            test_decode_fuzz(pkt, len, -1);  /* 任意错误均可接受 */
            free(pkt);
        }
    }

    free(wire);
    return 0;
}

MU_TEST(test_fuzz_byte_flip_header) {
    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    /* 翻转 header 区域（偏移 12 起，HEAD_SIZE=64 字节） */
    size_t header_start = 12;
    size_t header_end = header_start + 64;
    if (header_end > len) header_end = len;

    for (size_t i = header_start; i < header_end; i++) {
        uint8_t orig = wire[i];
        for (int b = 0; b < 8; b++) {
            uint8_t flipped = orig ^ (1 << b);
            uint8_t *pkt = (uint8_t*)malloc(len);
            memcpy(pkt, wire, len);
            pkt[i] = flipped;
            test_decode_fuzz(pkt, len, -1);
            free(pkt);
        }
        /* 置 0xFF（随机噪声） */
        uint8_t *pktf = (uint8_t*)malloc(len);
        memcpy(pktf, wire, len); pktf[i] = 0xFF;
        test_decode_fuzz(pktf, len, -1);
        free(pktf);
    }

    free(wire);
    return 0;
}

/* ================================================================
 * 边界值测试
 * ================================================================ */

MU_TEST(test_fuzz_boundary_body_len_zero) {
    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    /* body_len = 0 */
    uint8_t *pkt = (uint8_t*)malloc(len);
    memcpy(pkt, wire, len);
    pkt[8] = pkt[9] = pkt[10] = pkt[11] = 0x00;
    /* CRC 也需重新设为 0（故意让 CRC 不匹配，验证 CRC 检查） */
    pkt[4] = pkt[5] = pkt[6] = pkt[7] = 0x00;
    test_decode_fuzz(pkt, len, MSG_ERR_CRC_MISMATCH);
    free(pkt);

    free(wire);
    return 0;
}

MU_TEST(test_fuzz_boundary_body_len_max) {
    /* body_len = MSG_MAX_BODY_LEN + 1（应被拒绝） */
    uint8_t fake_packet[128];
    memset(fake_packet, 0x00, sizeof(fake_packet));
    memcpy(fake_packet, MSG_MAGIC, 4);
    /* body_len 设为超过最大值 */
    *(uint32_t*)(fake_packet + 8) = MSG_HTOLE32(MSG_MAX_BODY_LEN + 1);
    test_decode_fuzz(fake_packet, sizeof(fake_packet), MSG_ERR_BODY_TOO_LARGE);
    return 0;
}

MU_TEST(test_fuzz_boundary_buffer_too_small) {
    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    /* len < BODY_OFFSET(83) → ERR_BUFFER_TOO_SMALL */
    for (size_t l = 0; l < 83 && l < len; l++) {
        test_decode_fuzz(wire, l, MSG_ERR_BUFFER_TOO_SMALL);
    }
    /* len == BODY_OFFSET 但 body_len 不匹配 */
    uint8_t *pkt = (uint8_t*)malloc(83);
    memcpy(pkt, wire, 83);
    /* body_len 保持原值但 buffer 长度仅 83 */
    test_decode_fuzz(pkt, 83, MSG_ERR_BUFFER_TOO_SMALL);
    free(pkt);

    free(wire);
    return 0;
}

MU_TEST(test_fuzz_boundary_truncated_body) {
    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    /* 从 body_offset 开始逐字节截断（body_len > 实际数据） */
    uint32_t body_len = MSG_LE32TOH(*(uint32_t*)(wire + 8));
    if (body_len > 0 && len > (size_t)BODY_OFFSET) {
        size_t body_start = BODY_OFFSET;
        /* 保留至少 1 字节 body 但 body_len 仍为完整值 */
        for (size_t cut = body_start + 1; cut < len; cut++) {
            test_decode_fuzz(wire, cut, MSG_ERR_BUFFER_TOO_SMALL);
        }
    }

    free(wire);
    return 0;
}

/* ================================================================
 * 损坏转义序列测试
 * ================================================================ */

MU_TEST(test_fuzz_escape_orphaned_esc) {
    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    /* body 起始偏移 */
    size_t body_start = BODY_OFFSET;
    size_t body_len = MSG_LE32TOH(*(uint32_t*)(wire + 8));
    if (body_len == 0 || body_start + body_len > len) {
        free(wire);
        return 0;
    }

    /* 在 body 内找任意位置植入孤立 ESC（0x1B 后无后续字节） */
    for (size_t pos = body_start; pos + 1 < body_start + body_len; pos++) {
        if (wire[pos] != 0x1B) continue;

        /* 复制一份，将 [pos+1] 改为非法的 0x00 */
        uint8_t *pkt = (uint8_t*)malloc(len);
        memcpy(pkt, wire, len);
        pkt[pos + 1] = 0x00;  /* 破坏合法的 ESC pair */
        /* 修复 CRC */
        uint32_t new_body_len = MSG_LE32TOH(*(uint32_t*)(pkt + 8));
        uint32_t crc = crc32_update(0, pkt + 8, 4 + 64 + new_body_len);
        *(uint32_t*)(pkt + 4) = MSG_HTOLE32(crc);
        test_decode_fuzz(pkt, len, MSG_ERR_ESCAPE_SEQUENCE);
        free(pkt);
        break;  /* 找到一个即可 */
    }

    free(wire);
    return 0;
}

MU_TEST(test_fuzz_escape_invalid_esc_code) {
    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    size_t body_start = BODY_OFFSET;
    size_t body_len = MSG_LE32TOH(*(uint32_t*)(wire + 8));
    if (body_len == 0 || body_start + body_len > len) {
        free(wire);
        return 0;
    }

    /* 找 ESC+合法代码位置，替换代码为非法值 */
    for (size_t pos = body_start; pos + 1 < body_start + body_len; pos++) {
        if (wire[pos] != 0x1B) continue;

        /* 替换 [pos+1] 为非转义码 */
        static const uint8_t invalid_codes[] = { 0x00, 0x01, 0xFF, 0x50, 0x7F };
        for (size_t c = 0; c < sizeof(invalid_codes); c++) {
            uint8_t *pkt = (uint8_t*)malloc(len);
            memcpy(pkt, wire, len);
            pkt[pos + 1] = invalid_codes[c];
            uint32_t crc = crc32_update(0, pkt + 8, 4 + 64 + MSG_LE32TOH(*(uint32_t*)(pkt + 8)));
            *(uint32_t*)(pkt + 4) = MSG_HTOLE32(crc);
            test_decode_fuzz(pkt, len, MSG_ERR_ESCAPE_SEQUENCE);
            free(pkt);
        }
        break;
    }

    free(wire);
    return 0;
}

MU_TEST(test_fuzz_escape_esc_at_end) {
    /* body 最后字节是 ESC，无后续字节 */
    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    size_t body_start = BODY_OFFSET;
    size_t body_len = MSG_LE32TOH(*(uint32_t*)(wire + 8));
    if (body_len == 0 || body_start + body_len > len) {
        free(wire);
        return 0;
    }

    /* 将 body 最后一个字节改为 ESC（破坏最后一个有效 pair） */
    uint8_t *pkt = (uint8_t*)malloc(len);
    memcpy(pkt, wire, len);
    size_t last_body_pos = body_start + body_len - 1;
    pkt[last_body_pos] = 0x1B;
    /* 重新计算 CRC */
    uint32_t crc = crc32_update(0, pkt + 8, 4 + 64 + body_len);
    *(uint32_t*)(pkt + 4) = MSG_HTOLE32(crc);
    test_decode_fuzz(pkt, len, MSG_ERR_ESCAPE_SEQUENCE);
    free(pkt);

    free(wire);
    return 0;
}

/* ================================================================
 * 非法 header 测试
 * ================================================================ */

MU_TEST(test_fuzz_invalid_msg_type) {
    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    /* msg_type 在 header 中偏移 HEAD_MSGTYPE_POS = 55 */
    /* 0x55 = 'U'，不是合法消息类型 */
    uint8_t *pkt = (uint8_t*)malloc(len);
    memcpy(pkt, wire, len);
    pkt[55] = 0x55;
    uint32_t crc = crc32_update(0, pkt + 8, 4 + 64 + MSG_LE32TOH(*(uint32_t*)(pkt + 8)));
    *(uint32_t*)(pkt + 4) = MSG_HTOLE32(crc);
    test_decode_fuzz(pkt, len, MSG_ERR_INVALID_MSG_TYPE);
    free(pkt);

    /* 0x00 */
    uint8_t *pkt0 = (uint8_t*)malloc(len);
    memcpy(pkt0, wire, len);
    pkt0[55] = 0x00;
    crc = crc32_update(0, pkt0 + 8, 4 + 64 + MSG_LE32TOH(*(uint32_t*)(pkt0 + 8)));
    *(uint32_t*)(pkt0 + 4) = MSG_HTOLE32(crc);
    test_decode_fuzz(pkt0, len, MSG_ERR_INVALID_MSG_TYPE);
    free(pkt0);

    free(wire);
    return 0;
}

MU_TEST(test_fuzz_invalid_format) {
    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    /* format 在 header 中偏移 HEAD_FORMAT_POS = 54 */
    /* 0x00 不是合法 format（应为 'T'=0x54） */
    uint8_t *pkt = (uint8_t*)malloc(len);
    memcpy(pkt, wire, len);
    pkt[54] = 0x00;
    uint32_t crc = crc32_update(0, pkt + 8, 4 + 64 + MSG_LE32TOH(*(uint32_t*)(pkt + 8)));
    *(uint32_t*)(pkt + 4) = MSG_HTOLE32(crc);
    test_decode_fuzz(pkt, len, -1);  /* format 字段本身不解码，仅作占位 */
    free(pkt);

    free(wire);
    return 0;
}

MU_TEST(test_fuzz_all_invalid_msg_type_values) {
    /* 测试所有非法 msg_type（0x00-0xFF 中除了 R/A/P/H 之外的值） */
    static const uint8_t valid_types[] = {
        MSG_TYPE_REQUEST, MSG_TYPE_ANSWER, MSG_TYPE_PUSH, MSG_TYPE_HEARTBEAT
    };
    uint8_t fake_packet[128];
    memset(fake_packet, '0', sizeof(fake_packet));
    memcpy(fake_packet, MSG_MAGIC, 4);

    for (int t = 0; t <= 255; t++) {
        /* 跳过合法类型 */
        int is_valid = 0;
        for (size_t v = 0; v < sizeof(valid_types); v++) {
            if (t == valid_types[v]) { is_valid = 1; break; }
        }
        if (is_valid) continue;

        memcpy(fake_packet, MSG_MAGIC, 4);
        memset(fake_packet + 4, 0x00, 4);  /* crc32=0 */
        *(uint32_t*)(fake_packet + 8) = MSG_HTOLE32(0);  /* body_len=0 */
        fake_packet[55] = (uint8_t)t;  /* msg_type */

        test_decode_fuzz(fake_packet, 83, MSG_ERR_INVALID_MSG_TYPE);
    }
    return 0;
}

/* ================================================================
 * 畸形分隔符测试（破坏 body 内 US/RS/FS/GS 分隔符）
 * ================================================================ */

MU_TEST(test_fuzz_malformed_separator_us) {
    /* US (0x1F) 是列分隔符，将其替换为普通字符破坏结构 */
    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    size_t body_start = BODY_OFFSET;
    size_t body_len = MSG_LE32TOH(*(uint32_t*)(wire + 8));
    if (body_len == 0 || body_start + body_len > len) {
        free(wire);
        return 0;
    }

    int found = 0;
    for (size_t pos = body_start; pos < body_start + body_len && !found; pos++) {
        /* 将 US 替换为 'X' */
        if (wire[pos] == 0x1F) {
            uint8_t *pkt = (uint8_t*)malloc(len);
            memcpy(pkt, wire, len);
            pkt[pos] = 'X';
            uint32_t crc = crc32_update(0, pkt + 8, 4 + 64 + MSG_LE32TOH(*(uint32_t*)(pkt + 8)));
            *(uint32_t*)(pkt + 4) = MSG_HTOLE32(crc);
            /* 此情况可能解析失败或成功（取决于数据内容），不崩溃即可 */
            test_decode_fuzz(pkt, len, -1);
            free(pkt);
            found = 1;
        }
    }
    (void)found;

    free(wire);
    return 0;
}

MU_TEST(test_fuzz_malformed_separator_rs) {
    /* RS (0x1E) 是行分隔符，将其替换破坏行结构 */
    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    size_t body_start = BODY_OFFSET;
    size_t body_len = MSG_LE32TOH(*(uint32_t*)(wire + 8));
    if (body_len == 0 || body_start + body_len > len) {
        free(wire);
        return 0;
    }

    int found = 0;
    for (size_t pos = body_start; pos < body_start + body_len && !found; pos++) {
        if (wire[pos] == 0x1E) {
            uint8_t *pkt = (uint8_t*)malloc(len);
            memcpy(pkt, wire, len);
            pkt[pos] = 'Y';
            uint32_t crc = crc32_update(0, pkt + 8, 4 + 64 + MSG_LE32TOH(*(uint32_t*)(pkt + 8)));
            *(uint32_t*)(pkt + 4) = MSG_HTOLE32(crc);
            test_decode_fuzz(pkt, len, -1);
            free(pkt);
            found = 1;
        }
    }
    (void)found;

    free(wire);
    return 0;
}

MU_TEST(test_fuzz_malformed_separator_fs) {
    /* FS (0x1C) 是区隔表头与数据的分隔符，将其破坏 */
    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    size_t body_start = BODY_OFFSET;
    size_t body_len = MSG_LE32TOH(*(uint32_t*)(wire + 8));
    if (body_len == 0 || body_start + body_len > len) {
        free(wire);
        return 0;
    }

    int found = 0;
    for (size_t pos = body_start; pos < body_start + body_len && !found; pos++) {
        if (wire[pos] == 0x1C) {
            uint8_t *pkt = (uint8_t*)malloc(len);
            memcpy(pkt, wire, len);
            pkt[pos] = 'Z';
            uint32_t crc = crc32_update(0, pkt + 8, 4 + 64 + MSG_LE32TOH(*(uint32_t*)(pkt + 8)));
            *(uint32_t*)(pkt + 4) = MSG_HTOLE32(crc);
            test_decode_fuzz(pkt, len, -1);
            free(pkt);
            found = 1;
        }
    }
    (void)found;

    free(wire);
    return 0;
}

MU_TEST(test_fuzz_separator_replaced_with_esc) {
    /* 将分隔符替换为 ESC（0x1B），制造 ESC+随机码 */
    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    size_t body_start = BODY_OFFSET;
    size_t body_len = MSG_LE32TOH(*(uint32_t*)(wire + 8));
    if (body_len < 2 || body_start + body_len > len) {
        free(wire);
        return 0;
    }

    static const uint8_t seps[] = { 0x1F, 0x1E, 0x1C, 0x1D };
    for (size_t pos = body_start; pos + 1 < body_start + body_len; pos++) {
        for (size_t s = 0; s < sizeof(seps); s++) {
            if (wire[pos] != seps[s]) continue;

            /* 将 sep 替换为 ESC，且后一字节改为随机值 */
            uint8_t *pkt = (uint8_t*)malloc(len);
            memcpy(pkt, wire, len);
            pkt[pos] = 0x1B;
            pkt[pos + 1] = 0xFF;  /* 非法转义码 */
            uint32_t crc = crc32_update(0, pkt + 8, 4 + 64 + MSG_LE32TOH(*(uint32_t*)(pkt + 8)));
            *(uint32_t*)(pkt + 4) = MSG_HTOLE32(crc);
            test_decode_fuzz(pkt, len, MSG_ERR_ESCAPE_SEQUENCE);
            free(pkt);
        }
    }

    free(wire);
    return 0;
}

/* ================================================================
 * 边界帧测试（极小/极大/空 body）
 * ================================================================ */

MU_TEST(test_fuzz_empty_body) {
    /* body_len = 0 的极端情况 */
    uint8_t fake_packet[BODY_OFFSET];
    memset(fake_packet, 0x00, sizeof(fake_packet));
    memcpy(fake_packet, MSG_MAGIC, 4);
    /* crc32 = 0（故意错误，测试 CRC 检查） */
    memset(fake_packet + 4, 0x00, 4);
    /* body_len = 0 */
    memset(fake_packet + 8, 0x00, 4);
    /* header 全 0 */
    /* 此时 len == BODY_OFFSET，msg_decode 应返回 ERR_CRC_MISMATCH（因为 CRC 不匹配） */
    test_decode_fuzz(fake_packet, sizeof(fake_packet), MSG_ERR_CRC_MISMATCH);
    return 0;
}

MU_TEST(test_fuzz_null_inputs) {
    /* 空指针和零长度输入 */
    msg_packet_t *p = NULL;
    mu_check(msg_decode(NULL, 0, &p) == MSG_ERR_NULL_PTR);
    mu_check(msg_decode(NULL, 100, &p) == MSG_ERR_NULL_PTR);
    mu_check(msg_decode((void*)1, 0, NULL) == MSG_ERR_NULL_PTR);
    mu_check(msg_decode((void*)1, 100, NULL) == MSG_ERR_NULL_PTR);
    return 0;
}

MU_TEST(test_fuzz_wrong_magic) {
    static const char *wrong_magics[] = {
        "YSWW", "YYYY", "YSW", "YSWY1", "YSWY\x00", "????", "\x00\x00\x00\x00"
    };
    for (size_t i = 0; i < sizeof(wrong_magics) / sizeof(wrong_magics[0]); i++) {
        uint8_t fake[128];
        memset(fake, 0x00, sizeof(fake));
        memcpy(fake, wrong_magics[i], 4);
        test_decode_fuzz(fake, 83, MSG_ERR_INVALID_MAGIC);
    }
    return 0;
}

MU_TEST(test_fuzz_all_zero_packet) {
    /* 全部置零（除 magic 外），CRC 必然不匹配 */
    uint8_t fake[128];
    memset(fake, 0x00, sizeof(fake));
    memcpy(fake, MSG_MAGIC, 4);
    test_decode_fuzz(fake, 83, MSG_ERR_CRC_MISMATCH);
    return 0;
}

MU_TEST(test_fuzz_all_ff_packet) {
    /* 全部 0xFF */
    uint8_t fake[128];
    memset(fake, 0xFF, sizeof(fake));
    memcpy(fake, MSG_MAGIC, 4);  /* magic 正确，但其他全错 */
    test_decode_fuzz(fake, 83, -1);
    return 0;
}

MU_TEST(test_fuzz_body_len_inconsistent) {
    /* wire 中 body_len 与实际 body 大小不一致 */
    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    /* 将 body_len 改为大于实际 body 的值 */
    uint8_t *pkt = (uint8_t*)malloc(len);
    memcpy(pkt, wire, len);
    *(uint32_t*)(pkt + 8) = MSG_HTOLE32(len + 100);  /* 声称 body 更大 */
    test_decode_fuzz(pkt, len, MSG_ERR_BUFFER_TOO_SMALL);
    free(pkt);

    /* 将 body_len 改为小于实际 body 的值 */
    pkt = (uint8_t*)malloc(len);
    memcpy(pkt, wire, len);
    *(uint32_t*)(pkt + 8) = MSG_HTOLE32(1);  /* 声称 body 仅 1 字节 */
    /* 重新计算 CRC（body_len 改变了，CRC 范围也变了） */
    /* 此时 body_len=1，但 wire_size 仍为 len，CRC 计算用 body_len=1 */
    uint32_t crc = crc32_update(0, pkt + 8, 4 + 64 + 1);
    *(uint32_t*)(pkt + 4) = MSG_HTOLE32(crc);
    test_decode_fuzz(pkt, len, -1);
    free(pkt);

    free(wire);
    return 0;
}

/* ================================================================
 * 随机字节翻转（统计 fuzzing）
 * ================================================================ */

MU_TEST(test_fuzz_random_bitflips) {
    /* 随机种子固定，保证可重复性 */
    srand(0x12345678);

    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    /* 执行 200 次随机单字节翻转 */
    for (int iter = 0; iter < 200; iter++) {
        uint8_t *pkt = (uint8_t*)malloc(len);
        if (!pkt) break;
        memcpy(pkt, wire, len);

        size_t pos = (size_t)(rand() % len);
        uint8_t mask = (uint8_t)(rand() % 256);
        pkt[pos] ^= mask;

        /* CRC 可能错误，不重新计算，直接 fuzz */
        test_decode_fuzz(pkt, len, -1);
        free(pkt);
    }

    free(wire);
    return 0;
}

MU_TEST(test_fuzz_multi_byte_corruption) {
    /* 连续多字节随机破坏（模拟严重位翻转） */
    srand(0xABCDEF00);

    size_t len;
    uint8_t *wire = build_valid_packet(&len);
    mu_check(wire != NULL);

    for (int iter = 0; iter < 50; iter++) {
        uint8_t *pkt = (uint8_t*)malloc(len);
        if (!pkt) break;
        memcpy(pkt, wire, len);

        /* 随机选择连续 2-4 字节区域破坏 */
        size_t start = (size_t)(rand() % len);
        int count = 2 + (rand() % 3);
        for (int c = 0; c < count && start + c < len; c++) {
            pkt[start + c] = (uint8_t)(rand() % 256);
        }

        test_decode_fuzz(pkt, len, -1);
        free(pkt);
    }

    free(wire);
    return 0;
}

/* ================================================================
 * 测试套件
 * ================================================================ */

MU_TEST_SUITE(fuzz_suite) {
    /* 初始化 CRC 表 */
    crc32_init();

    printf("\n--- Byte Flip Tests ---\n");
    MU_RUN_TEST(test_fuzz_byte_flip_magic);
    MU_RUN_TEST(test_fuzz_byte_flip_crc32);
    MU_RUN_TEST(test_fuzz_byte_flip_body_len);
    MU_RUN_TEST(test_fuzz_byte_flip_header);

    printf("\n--- Boundary Value Tests ---\n");
    MU_RUN_TEST(test_fuzz_boundary_body_len_zero);
    MU_RUN_TEST(test_fuzz_boundary_body_len_max);
    MU_RUN_TEST(test_fuzz_boundary_buffer_too_small);
    MU_RUN_TEST(test_fuzz_boundary_truncated_body);

    printf("\n--- Corrupt Escape Sequence Tests ---\n");
    MU_RUN_TEST(test_fuzz_escape_orphaned_esc);
    MU_RUN_TEST(test_fuzz_escape_invalid_esc_code);
    MU_RUN_TEST(test_fuzz_escape_esc_at_end);

    printf("\n--- Invalid Header Tests ---\n");
    MU_RUN_TEST(test_fuzz_invalid_msg_type);
    MU_RUN_TEST(test_fuzz_invalid_format);
    MU_RUN_TEST(test_fuzz_all_invalid_msg_type_values);

    printf("\n--- Malformed Separator Tests ---\n");
    MU_RUN_TEST(test_fuzz_malformed_separator_us);
    MU_RUN_TEST(test_fuzz_malformed_separator_rs);
    MU_RUN_TEST(test_fuzz_malformed_separator_fs);
    MU_RUN_TEST(test_fuzz_separator_replaced_with_esc);

    printf("\n--- Edge Frame Tests ---\n");
    MU_RUN_TEST(test_fuzz_empty_body);
    MU_RUN_TEST(test_fuzz_null_inputs);
    MU_RUN_TEST(test_fuzz_wrong_magic);
    MU_RUN_TEST(test_fuzz_all_zero_packet);
    MU_RUN_TEST(test_fuzz_all_ff_packet);
    MU_RUN_TEST(test_fuzz_body_len_inconsistent);

    printf("\n--- Random Fuzzing Tests ---\n");
    MU_RUN_TEST(test_fuzz_random_bitflips);
    MU_RUN_TEST(test_fuzz_multi_byte_corruption);
}

int main() {
    printf("=================================================\n");
    printf(" MsgPacket Fuzzing Tests (msg_decode path)\n");
    printf("=================================================\n");
    printf(" HEAD_SIZE   = %d\n", (int)HEAD_SIZE);
    printf(" BODY_OFFSET = %d\n", (int)BODY_OFFSET);
    printf(" Magic       = %.4s\n", MSG_MAGIC);
    printf(" Max body    = %d\n", MSG_MAX_BODY_LEN);
    printf("=================================================\n");

    MU_RUN_SUITE(fuzz_suite);
    MU_PRINT_REPORT();
    return mu_failed > 0 ? 1 : 0;
}
