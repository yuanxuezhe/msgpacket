#ifndef MSG_INTERNAL_H
#define MSG_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  /* strcasecmp */
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#include <time.h>
#endif
#include "msg_api.h"    // for msg_packet_t, MSG_ERR_*, MSG_SEP_*, HEAD_*, MSG_*, MSG_MAX_*, MSG_MAGIC, MSG_FORMAT_*, MSG_ERR_NO_MEMORY
#include "msg_util.h"  // for msg_generate_uuid_v4, crc32_init

/* 生成当前时间戳字符串 yyyyMMddHHmmssSSS（固定 17 字节，写入 HEAD_TIMESTAMP_LENGTH 字节） */
static void generate_timestamp_str(char out[HEAD_TIMESTAMP_LENGTH]) {
    char tmp[64];
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(tmp, sizeof(tmp), "%04d%02d%02d%02d%02d%02d%03d",
             (int)st.wYear, (int)st.wMonth, (int)st.wDay,
             (int)st.wHour, (int)st.wMinute, (int)st.wSecond,
             (int)st.wMilliseconds);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *t = localtime(&tv.tv_sec);
    snprintf(tmp, sizeof(tmp), "%04d%02d%02d%02d%02d%02d%03d",
             (int)(t->tm_year + 1900), (int)(t->tm_mon + 1), (int)t->tm_mday,
             (int)t->tm_hour, (int)t->tm_min, (int)t->tm_sec,
             (int)(tv.tv_usec / 1000));
#endif
    memcpy(out, tmp, HEAD_TIMESTAMP_LENGTH);  /* 固定拷贝 18 字节（不含 \0） */
}

/* ============================================ */
/* 内部结构 */
/* ============================================ */

/* 单个结果集（构建阶段和解码后均使用相同结构） */
typedef struct {
    /* 构建阶段 */
    char   **headers;         /* 表头名称数组 */
    size_t   header_count;
    char   ***rows;           /* 行数据：rows[row][col] = 字段值字符串指针 */
    size_t   row_count;

    /* 解码后：字段描述符 */
} result_set_t;

/* 内部状态（不与线上结构体混在一起） */
typedef struct {
    /* 多结果集支持（动态数组） */
    result_set_t *result_sets;  /* 结果集数组 */
    size_t        rs_count;     /* 已添加的结果集数量（最小 1） */
    size_t        rs_cap;       /* result_sets 容量 */
    size_t        current_rs;   /* 当前活跃结果集索引（0-based） */

    /* wire 缓冲区（finalize 后或 decode 后） */
    uint8_t *wire_buf;
    size_t   wire_size;

    /* unescaped body（解码后整个 body 的转义还原数据） */
    uint8_t *unescaped_body;
    size_t   unescaped_len;

    /* 遍历游标 */
    size_t   cursor_row;      /* SIZE_MAX = 未开始 */
} msg_internal_t;

/* 按需扩容 result_set_t 数组 */
static int ensure_rs_cap(msg_internal_t *in, size_t needed) {
    if (needed <= in->rs_cap) return 0;
    size_t new_cap = in->rs_cap ? in->rs_cap * 2 : 4;
    while (new_cap < needed) new_cap *= 2;
    result_set_t *tmp = (result_set_t*)realloc(in->result_sets, sizeof(result_set_t) * new_cap);
    if (!tmp) return MSG_ERR_NO_MEMORY;
    /* 初始化新增槽位 */
    for (size_t i = in->rs_cap; i < new_cap; i++) {
        memset(&tmp[i], 0, sizeof(result_set_t));
    }
    in->result_sets = tmp;
    in->rs_cap = new_cap;
    return 0;
}

/* ============================================ */
/* 内部辅助：内存布局 */
/* ============================================ */

/*
 * msg_packet_t 分配布局（内部指针在结构体之前）：
 *   [msg_internal_t* (sizeof(void*) 字节)] [msg_packet_t]
 *                                           ^-- 返回给用户的指针
 * 这样 msg_packet_t 只包含线上字段，不与运行时状态混淆。
 */

static msg_internal_t* internal_get(const msg_packet_t *packet) {
    if (!packet) return NULL;
    return *(msg_internal_t**)(((char*)packet) - sizeof(void*));
}

static void internal_set(msg_packet_t *packet, msg_internal_t *in) {
    *(msg_internal_t**)(((char*)packet) - sizeof(void*)) = in;
}

static msg_packet_t* packet_alloc(msg_internal_t *in) {
    char *raw = (char*)calloc(1, sizeof(void*) + sizeof(msg_packet_t));
    if (!raw) return NULL;
    msg_packet_t *p = (msg_packet_t*)(raw + sizeof(void*));
    internal_set(p, in);
    return p;
}

/* 获取当前结果集指针（构建阶段） */
static result_set_t* current_rs_build(const msg_internal_t *in) {
    if (!in || in->rs_count == 0) return NULL;
    return &in->result_sets[in->current_rs];
}

/* msg_destroy 的底层实现：释放 internal + packet 块 */
static void packet_free_internal(msg_packet_t *packet, msg_internal_t *in) {
    if (!packet) return;
    if (in) {
        /* 释放所有结果集 */
        for (size_t i = 0; i < in->rs_count; i++) {
            result_set_t *rs = &in->result_sets[i];
            for (size_t j = 0; j < rs->header_count; j++) free(rs->headers[j]);
            free(rs->headers);
            for (size_t j = 0; j < rs->row_count; j++) {
                for (size_t k = 0; k < rs->header_count; k++) free(rs->rows[j][k]);
                free(rs->rows[j]);
            }
            free(rs->rows);
        }
        free(in->result_sets);
        /* 释放 wire 缓冲区 */
        free(in->wire_buf);
        /* 释放 unescaped body */
        free(in->unescaped_body);
        free(in);
    }
    free((char*)packet - sizeof(void*));
}

/* ============================================ */
/* 内部辅助：校验与查找 */
/* ============================================ */

static bool msg_is_valid_type(uint8_t t) {
    return t == MSG_TYPE_REQUEST || t == MSG_TYPE_ANSWER ||
           t == MSG_TYPE_PUSH || t == MSG_TYPE_HEARTBEAT;
}

/* 按 key 查找列索引（大小写不敏感，完整匹配） */
static int internal_find_col(const msg_internal_t *in, const char *key) {
    if (!key || in->rs_count == 0) return -1;
    result_set_t *rs = &in->result_sets[in->current_rs];
    if (!rs->headers) return -1;
    for (size_t i = 0; i < rs->header_count; i++) {
        if (strcasecmp(rs->headers[i], key) == 0)
            return (int)i;
    }
    return -1;
}

/* 在构建阶段的 headers 数组中按 key 查找列索引（大小写不敏感） */
static int internal_find_build_col(const msg_internal_t *in, const char *key) {
    if (!in || !key) return -1;
    result_set_t *rs = current_rs_build(in);
    if (!rs || !rs->headers) return -1;
    for (size_t i = 0; i < rs->header_count; i++) {
        if (strcasecmp(rs->headers[i], key) == 0) return (int)i;
    }
    return -1;
}

#endif /* MSG_INTERNAL_H */
