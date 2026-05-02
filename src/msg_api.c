#include "msg_packet.h"
#include "msg_util.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <stdbool.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

#ifdef _MSC_VER
#define msg_strcasecmp  _stricmp
#define msg_strncasecmp _strnicmp
#else
#include <strings.h>
#define msg_strcasecmp  strcasecmp
#define msg_strncasecmp strncasecmp
#endif

/* 生成当前时间戳字符串 yyyyMMddHHmmssSSS（17 位，无 \0） */
static void generate_timestamp_str(char out[17]) {
    char tmp[18];
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(tmp, sizeof(tmp), "%04d%02d%02d%02d%02d%02d%03d",
             st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *t = localtime(&tv.tv_sec);
    snprintf(tmp, sizeof(tmp), "%04d%02d%02d%02d%02d%02d%03d",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec,
             (int)(tv.tv_usec / 1000));
#endif
    memcpy(out, tmp, 17);  /* 仅拷贝 17 字节，不含 \0 */
}

/* ============================================ */
/* 内部结构 */
/* ============================================ */

/* 字段描述符 */
typedef struct {
    size_t offset;
    size_t len;
} field_desc_t;

/* 内部状态（不与线上结构体混在一起） */
typedef struct {
    bool     finalized;       /* true after msg_finalize or msg_decode */

    /* 构建阶段 */
    char   **headers;         /* 表头名称数组 */
    size_t   header_count;
    char   **rows;            /* 行数据（逗号分隔字符串） */
    size_t   row_count;

    /* wire 缓冲区（finalize 后或 decode 后） */
    uint8_t *wire_buf;
    size_t   wire_size;

    /* unescaped body + 字段索引（decode 后解析） */
    uint8_t      *unescaped_body;
    size_t        unescaped_len;
    field_desc_t *header_fields;
    size_t        header_field_count;
    size_t        header_field_cap;
    field_desc_t *data_rows;
    size_t        data_row_count;
    size_t        data_row_cap;

    /* 遍历游标 */
    size_t   cursor_row;      /* SIZE_MAX = 未开始 */
} msg_internal_t;

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

/* msg_destroy 的底层实现：释放 internal + packet 块 */
static void packet_free_internal(msg_packet_t *packet, msg_internal_t *in) {
    if (!packet) return;
    if (in) {
        for (size_t i = 0; i < in->header_count; i++) free(in->headers[i]);
        free(in->headers);
        for (size_t i = 0; i < in->row_count; i++) free(in->rows[i]);
        free(in->rows);
        free(in->wire_buf);
        free(in->unescaped_body);
        free(in->header_fields);
        free(in->data_rows);
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

/* 在 unescaped_body 解析结果中按 key 查找列索引（大小写不敏感）
 * 直接基于原始数据进行 strncasecmp，无长度截断 */
static int internal_find_col(const msg_internal_t *in, const char *key) {
    if (!key || !in->header_fields || !in->unescaped_body) return -1;
    size_t keylen = strlen(key);
    for (size_t i = 0; i < in->header_field_count; i++) {
        size_t hlen = in->header_fields[i].len;
        if (keylen != hlen) continue;
        if (msg_strncasecmp((const char*)(in->unescaped_body + in->header_fields[i].offset),
                            key, hlen) == 0)
            return (int)i;
    }
    return -1;
}

/* 在构建阶段的 headers 数组中按 key 查找列索引（大小写不敏感） */
static int internal_find_build_col(const msg_internal_t *in, const char *key) {
    if (!key || !in->headers) return -1;
    for (size_t i = 0; i < in->header_count; i++) {
        if (msg_strcasecmp(in->headers[i], key) == 0) return (int)i;
    }
    return -1;
}

static void internal_free_parsed(msg_internal_t *in) {
    free(in->unescaped_body);  in->unescaped_body = NULL;
    free(in->header_fields);   in->header_fields = NULL;
    free(in->data_rows);       in->data_rows = NULL;
    in->unescaped_len = 0;
    in->header_field_count = 0;
    in->header_field_cap = 0;
    in->data_row_count = 0;
    in->data_row_cap = 0;
}

/* 按需扩容 field_desc_t 数组（指数增长，减少 realloc 次数） */
static int ensure_field_cap(field_desc_t **arr, size_t *cap, size_t needed) {
    if (needed <= *cap) return 0;
    size_t new_cap = *cap ? *cap * 2 : 32;
    while (new_cap < needed) new_cap *= 2;
    field_desc_t *tmp = (field_desc_t*)realloc(*arr, sizeof(field_desc_t) * new_cap);
    if (!tmp) return MSG_ERR_NO_MEMORY;
    *arr = tmp;
    *cap = new_cap;
    return 0;
}

static int internal_parse_body(msg_internal_t *in) {
    if (!in->unescaped_body || in->unescaped_len == 0) return 0;

    /* 单次扫描：查找 FS 的同时解析表头字段（US 分隔） */
    size_t h_start = 0;
    size_t fs_offset = 0;
    bool found_fs = false;
    for (size_t i = 0; i < in->unescaped_len; i++) {
        uint8_t ch = in->unescaped_body[i];
        if (ch == MSG_SEP_SECTION || ch == MSG_SEP_COL) {
            if (ensure_field_cap(&in->header_fields, &in->header_field_cap,
                                in->header_field_count + 1) != 0)
                return MSG_ERR_NO_MEMORY;
            in->header_fields[in->header_field_count].offset = h_start;
            in->header_fields[in->header_field_count].len = i - h_start;
            in->header_field_count++;
            if (ch == MSG_SEP_SECTION) { fs_offset = i; found_fs = true; break; }
            h_start = i + 1;
        }
    }
    if (!found_fs) return MSG_ERR_INVALID_FORMAT;  /* 有数据但找不到 FS 分隔符 */

    /* 解析数据行（每行一个 field_desc_t，offset/len 覆盖整行） */
    size_t d_start = fs_offset + 1;
    for (size_t i = d_start; i <= in->unescaped_len; i++) {
        if (i == in->unescaped_len || in->unescaped_body[i] == MSG_SEP_ROW) {
            if (ensure_field_cap(&in->data_rows, &in->data_row_cap,
                                in->data_row_count + 1) != 0)
                return MSG_ERR_NO_MEMORY;
            in->data_rows[in->data_row_count].offset = d_start;
            in->data_rows[in->data_row_count].len = i - d_start;
            in->data_row_count++;
            d_start = i + 1;
        }
    }

    return 0;
}

/*
 * 在行数据中定位第 target_col 个字段（按 US/逗号 分隔）
 * 返回 true 表示找到，写入 *out_start 和 *out_len
 * 被 msg_get_value_str 和 msg_get_field 共享
 */
static bool row_get_field_at(const uint8_t *body, size_t row_offset, size_t row_len,
                              size_t target_col, size_t *out_start, size_t *out_len) {
    size_t field_start = row_offset;
    size_t field_idx = 0;
    for (size_t i = 0; i < row_len; i++) {
        uint8_t ch = body[row_offset + i];
        if (ch == MSG_SEP_COL || ch == ',') {
            if (field_idx == target_col) {
                *out_start = field_start;
                *out_len = (row_offset + i) - field_start;
                return true;
            }
            field_idx++;
            field_start = row_offset + i + 1;
        }
    }
    if (field_idx == target_col) {
        *out_start = field_start;
        *out_len = (row_offset + row_len) - field_start;
        return true;
    }
    return false;
}

/*
 * 确保缓冲区容量足够。msg_finalize 中重复的扩容逻辑统一到此函数。
 */
static int ensure_body_capacity(uint8_t **buf, size_t *cap, size_t needed) {
    while (needed >= *cap) {
        size_t new_cap = *cap * 2;
        uint8_t *tmp = (uint8_t*)realloc(*buf, new_cap);
        if (!tmp) return MSG_ERR_NO_MEMORY;
        *buf = tmp;
        *cap = new_cap;
    }
    return 0;
}

/* ============================================ */
/* 创建与销毁 */
/* ============================================ */

msg_packet_t* msg_create(uint8_t msg_type, const char *version) {
    if (!msg_is_valid_type(msg_type)) return NULL;

    msg_internal_t *in = (msg_internal_t*)calloc(1, sizeof(msg_internal_t));
    if (!in) return NULL;

    msg_packet_t *packet = packet_alloc(in);
    if (!packet) { free(in); return NULL; }

    /* magic */
    memcpy(packet->magic, MSG_MAGIC, 4);
    packet->crc32 = 0;

    /* UUID v4 */
    msg_generate_uuid_v4(packet->header.msg_id);

    /* version */
    msg_copy_fixed_field(packet->header.ver,
        version ? version : MSG_VERSION_DEFAULT, 8);

    packet->header.format = MSG_FORMAT_TABLE;
    packet->header.msg_type = msg_type;
    generate_timestamp_str(packet->header.timestamp);
    msg_copy_fixed_field(packet->header.func, "", 8);
    memcpy(packet->header.msg_code, MSG_CODE_SUCCESS, 5);

    in->cursor_row = (size_t)-1;
    crc32_init();
    return packet;
}

void msg_destroy(msg_packet_t *packet) {
    if (!packet) return;
    msg_internal_t *in = internal_get(packet);
    packet_free_internal(packet, in);
}

msg_packet_t* msg_clone(const msg_packet_t *packet) {
    if (!packet) return NULL;
    msg_internal_t *old_in = internal_get(packet);
    if (!old_in) return NULL;

    msg_internal_t *new_in = (msg_internal_t*)calloc(1, sizeof(msg_internal_t));
    if (!new_in) return NULL;

    msg_packet_t *clone = packet_alloc(new_in);
    if (!clone) { free(new_in); return NULL; }

    /* 复制 header */
    memcpy(clone->magic, packet->magic, 4);
    clone->crc32 = packet->crc32;
    clone->body_len = packet->body_len;
    memcpy(&clone->header, &packet->header, HEAD_SIZE);

    /* 复制内部状态 */
    new_in->finalized = old_in->finalized;

    /* headers */
    if (old_in->header_count > 0) {
        new_in->headers = (char**)malloc(sizeof(char*) * old_in->header_count);
        if (!new_in->headers) goto clone_fail;
        new_in->header_count = old_in->header_count;
        for (size_t i = 0; i < old_in->header_count; i++) {
            new_in->headers[i] = (char*)malloc(strlen(old_in->headers[i]) + 1);
            if (!new_in->headers[i]) goto clone_fail;
            strcpy(new_in->headers[i], old_in->headers[i]);
        }
    }

    /* rows */
    if (old_in->row_count > 0) {
        new_in->rows = (char**)malloc(sizeof(char*) * old_in->row_count);
        if (!new_in->rows) goto clone_fail;
        new_in->row_count = old_in->row_count;
        for (size_t i = 0; i < old_in->row_count; i++) {
            if (old_in->rows[i]) {
                new_in->rows[i] = (char*)malloc(strlen(old_in->rows[i]) + 1);
                if (!new_in->rows[i]) goto clone_fail;
                strcpy(new_in->rows[i], old_in->rows[i]);
            } else {
                new_in->rows[i] = NULL;
            }
        }
    }

    /* wire buf */
    if (old_in->wire_buf && old_in->wire_size > 0) {
        new_in->wire_buf = (uint8_t*)malloc(old_in->wire_size);
        if (!new_in->wire_buf) goto clone_fail;
        memcpy(new_in->wire_buf, old_in->wire_buf, old_in->wire_size);
        new_in->wire_size = old_in->wire_size;
    }

    /* unescaped body + field descs */
    if (old_in->unescaped_body && old_in->unescaped_len > 0) {
        new_in->unescaped_body = (uint8_t*)malloc(old_in->unescaped_len);
        if (!new_in->unescaped_body) goto clone_fail;
        memcpy(new_in->unescaped_body, old_in->unescaped_body, old_in->unescaped_len);
        new_in->unescaped_len = old_in->unescaped_len;
    }
    if (old_in->header_field_count > 0) {
        new_in->header_fields = (field_desc_t*)malloc(
            sizeof(field_desc_t) * old_in->header_field_count);
        if (!new_in->header_fields) goto clone_fail;
        memcpy(new_in->header_fields, old_in->header_fields,
               sizeof(field_desc_t) * old_in->header_field_count);
        new_in->header_field_count = old_in->header_field_count;
        new_in->header_field_cap = old_in->header_field_count;
    }
    if (old_in->data_row_count > 0) {
        new_in->data_rows = (field_desc_t*)malloc(
            sizeof(field_desc_t) * old_in->data_row_count);
        if (!new_in->data_rows) goto clone_fail;
        memcpy(new_in->data_rows, old_in->data_rows,
               sizeof(field_desc_t) * old_in->data_row_count);
        new_in->data_row_count = old_in->data_row_count;
        new_in->data_row_cap = old_in->data_row_count;
    }

    new_in->cursor_row = old_in->cursor_row;
    return clone;

clone_fail:
    packet_free_internal(clone, new_in);
    return NULL;
}

/* ============================================ */
/* Header 字段设置 */
/* ============================================ */

int msg_set_msg_id(msg_packet_t *packet, const char *msg_id) {
    if (!packet || !msg_id) return MSG_ERR_NULL_PTR;
    msg_copy_fixed_field(packet->header.msg_id, msg_id, 32);
    return 0;
}

int msg_set_func(msg_packet_t *packet, const char *func) {
    if (!packet || !func) return MSG_ERR_NULL_PTR;
    msg_copy_fixed_field(packet->header.func, func, 8);
    return 0;
}

int msg_set_type(msg_packet_t *packet, uint8_t msg_type) {
    if (!packet) return MSG_ERR_NULL_PTR;
    if (!msg_is_valid_type(msg_type)) return MSG_ERR_INVALID_MSG_TYPE;
    packet->header.msg_type = msg_type;
    return 0;
}

int msg_set_code(msg_packet_t *packet, const char *code) {
    if (!packet) return MSG_ERR_NULL_PTR;
    if (!code) { memcpy(packet->header.msg_code, MSG_CODE_SUCCESS, 5); return 0; }
    size_t len = strlen(code);
    /* 右对齐，左补 '0' */
    int pad = 5 - (int)len;
    if (pad < 0) pad = 0;
    memset(packet->header.msg_code, '0', pad);
    memcpy(packet->header.msg_code + pad, code, len < 5 ? len : 5);
    return 0;
}

int msg_set_code_int(msg_packet_t *packet, int32_t code) {
    if (code < 0 || code > 99999) return MSG_ERR_INVALID_FORMAT;
    char buf[6];
    snprintf(buf, sizeof(buf), "%05d", code);
    return msg_set_code(packet, buf);
}

int msg_set_timestamp(msg_packet_t *packet, const char *timestamp) {
    if (!packet) return MSG_ERR_NULL_PTR;
    if (!timestamp || timestamp[0] == '\0') {
        generate_timestamp_str(packet->header.timestamp);
    } else {
        memcpy(packet->header.timestamp, timestamp, 17);
    }
    return 0;
}

int msg_set_format(msg_packet_t *packet, uint8_t format) {
    if (!packet) return MSG_ERR_NULL_PTR;
    packet->header.format = format;
    return 0;
}

int msg_set_version(msg_packet_t *packet, const char *version) {
    if (!packet) return MSG_ERR_NULL_PTR;
    msg_copy_fixed_field(packet->header.ver, version, 8);
    return 0;
}

/* ============================================ */
/* Header 字段获取 */
/* ============================================ */

const char* msg_get_msg_id(const msg_packet_t *packet) {
    return packet ? packet->header.msg_id : NULL;
}

const char* msg_get_func(const msg_packet_t *packet) {
    return packet ? packet->header.func : NULL;
}

const char* msg_get_version(const msg_packet_t *packet) {
    return packet ? packet->header.ver : NULL;
}

uint8_t msg_get_type(const msg_packet_t *packet) {
    return packet ? packet->header.msg_type : 0;
}

const char* msg_get_code(const msg_packet_t *packet) {
    return packet ? packet->header.msg_code : NULL;
}

const char* msg_get_timestamp(const msg_packet_t *packet) {
    return packet ? packet->header.timestamp : NULL;
}

uint8_t msg_get_format(const msg_packet_t *packet) {
    return packet ? packet->header.format : 0;
}

uint32_t msg_get_body_len(const msg_packet_t *packet) {
    return packet ? MSG_LE32TOH(packet->body_len) : 0;
}

size_t msg_get_total_len(const msg_packet_t *packet) {
    return packet ? (BODY_OFFSET + MSG_LE32TOH(packet->body_len)) : 0;
}

/* ============================================ */
/* 表头构建 */
/* ============================================ */

int msg_set_headers(msg_packet_t *packet, int column_count, const char *headers) {
    if (!packet || !headers) return MSG_ERR_NULL_PTR;
    if (column_count <= 0 || column_count > MSG_MAX_HEADERS) return MSG_ERR_TOO_MANY_HEADERS;

    msg_internal_t *in = internal_get(packet);
    if (!in) return MSG_ERR_NULL_PTR;

    /* 清除旧表头 */
    for (size_t i = 0; i < in->header_count; i++) free(in->headers[i]);
    free(in->headers);
    in->headers = NULL;
    in->header_count = 0;

    in->headers = (char**)calloc((size_t)column_count, sizeof(char*));
    if (!in->headers) return MSG_ERR_NO_MEMORY;

    /* 解析逗号分隔的表头 */
    char *copy = (char*)malloc(strlen(headers) + 1);
    if (!copy) {
        free(in->headers);
        in->headers = NULL;
        return MSG_ERR_NO_MEMORY;
    }
    strcpy(copy, headers);

    int ret = 0;
    size_t idx = 0;
    char *saveptr = NULL;
    char *token = strtok_r(copy, ",", &saveptr);
    while (token && idx < (size_t)column_count) {
        size_t tlen = strlen(token);
        if (tlen > MSG_MAX_FIELD_LEN) { ret = MSG_ERR_FIELD_TOO_LONG; goto cleanup; }
        in->headers[idx] = (char*)malloc(tlen + 1);
        if (!in->headers[idx]) { ret = MSG_ERR_NO_MEMORY; goto cleanup; }
        strcpy(in->headers[idx], token);
        idx++;
        token = strtok_r(NULL, ",", &saveptr);
    }
    in->header_count = idx;

    /* 验证解析出的表头数量与预期一致 */
    if (idx != (size_t)column_count) { ret = MSG_ERR_INVALID_FORMAT; goto cleanup; }

cleanup:
    free(copy);
    if (ret != 0) {
        for (size_t i = 0; i < idx; i++) free(in->headers[i]);
        free(in->headers);
        in->headers = NULL;
        in->header_count = 0;
    }
    return ret;
}

int msg_add_header(msg_packet_t *packet, const char *header) {
    if (!packet || !header) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in) return MSG_ERR_NULL_PTR;
    if (in->header_count >= MSG_MAX_HEADERS) return MSG_ERR_TOO_MANY_HEADERS;

    char **new_h = (char**)realloc(in->headers, sizeof(char*) * (in->header_count + 1));
    if (!new_h) return MSG_ERR_NO_MEMORY;
    in->headers = new_h;

    size_t hlen = strlen(header);
    if (hlen > MSG_MAX_FIELD_LEN) return MSG_ERR_FIELD_TOO_LONG;

    in->headers[in->header_count] = (char*)malloc(hlen + 1);
    if (!in->headers[in->header_count]) return MSG_ERR_NO_MEMORY;
    strcpy(in->headers[in->header_count], header);
    in->header_count++;
    return 0;
}

int msg_get_headers(const msg_packet_t *packet, char *out, size_t *out_len) {
    if (!packet || !out || !out_len) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in) return MSG_ERR_NULL_PTR;

    /* 从内部 header 数组重建逗号分隔字符串 */
    size_t needed = 0;
    for (size_t i = 0; i < in->header_count; i++) {
        needed += strlen(in->headers[i]);
        if (i > 0) needed++; /* comma */
    }
    if (needed >= *out_len) { *out_len = needed + 1; return MSG_ERR_BUFFER_TOO_SMALL; }

    size_t pos = 0;
    for (size_t i = 0; i < in->header_count; i++) {
        if (i > 0) out[pos++] = ',';
        size_t hlen = strlen(in->headers[i]);
        memcpy(out + pos, in->headers[i], hlen);
        pos += hlen;
    }
    out[pos] = '\0';
    *out_len = pos;
    return 0;
}

/* ============================================ */
/* 数据行构建 */
/* ============================================ */

int msg_begin_row(msg_packet_t *packet) {
    if (!packet) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in) return MSG_ERR_NULL_PTR;
    if (in->row_count >= MSG_MAX_ROWS) return MSG_ERR_TOO_MANY_ROWS;

    char **new_rows = (char**)realloc(in->rows, sizeof(char*) * (in->row_count + 1));
    if (!new_rows) return MSG_ERR_NO_MEMORY;
    in->rows = new_rows;
    in->rows[in->row_count] = NULL;
    in->row_count++;
    return 0;
}

int msg_set_row(msg_packet_t *packet, const char *fmt, ...) {
    if (!packet || !fmt) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in || in->row_count == 0) return MSG_ERR_NO_DATA;

    va_list args;
    va_start(args, fmt);

    size_t row_idx = in->row_count - 1;
    free(in->rows[row_idx]);
    in->rows[row_idx] = NULL;

    /* vsnprintf to get needed size */
    va_list args2;
    va_copy(args2, args);
    int needed = vsnprintf(NULL, 0, fmt, args2);
    va_end(args2);

    if (needed < 0) { va_end(args); return MSG_ERR_NO_MEMORY; }

    size_t buf_size = (size_t)needed + 1;
    in->rows[row_idx] = (char*)malloc(buf_size);
    if (!in->rows[row_idx]) { va_end(args); return MSG_ERR_NO_MEMORY; }

    vsnprintf(in->rows[row_idx], buf_size, fmt, args);
    va_end(args);

    /* 校验每个逗号分隔字段的长度 */
    {
        const char *s = in->rows[row_idx];
        size_t field_start = 0;
        for (size_t i = 0; ; i++) {
            if (s[i] == ',' || s[i] == '\0') {
                if (i - field_start > MSG_MAX_FIELD_LEN) {
                    free(in->rows[row_idx]);
                    in->rows[row_idx] = NULL;
                    return MSG_ERR_FIELD_TOO_LONG;
                }
                if (s[i] == '\0') break;
                field_start = i + 1;
            }
        }
    }
    return 0;
}

int msg_set_value_str(msg_packet_t *packet, const char *key, const char *value) {
    if (!packet || !key || !value) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in) return MSG_ERR_NULL_PTR;

    int col_idx = internal_find_build_col(in, key);
    if (col_idx < 0) {
        int rc = msg_add_header(packet, key);
        if (rc != 0) return rc;
        col_idx = (int)in->header_count - 1;
    }

    if (strlen(value) > MSG_MAX_FIELD_LEN) return MSG_ERR_FIELD_TOO_LONG;

    /* 确保有当前行 */
    if (in->row_count == 0) {
        int rc = msg_begin_row(packet);
        if (rc != 0) return rc;
    }

    size_t row_idx = in->row_count - 1;

    /* 解析现有行数据，保存各列已有值（堆分配，避免栈上 2KB VLA） */
    size_t max_cols = in->header_count > 0 ? in->header_count : 1;
    char **existing = (char**)calloc(max_cols, sizeof(char*));
    if (!existing) return MSG_ERR_NO_MEMORY;
    size_t exist_count = 0;
    if (in->rows[row_idx]) {
        char *copy = (char*)malloc(strlen(in->rows[row_idx]) + 1);
        if (copy) {
            strcpy(copy, in->rows[row_idx]);
            char *saveptr = NULL;
            char *token = strtok_r(copy, ",", &saveptr);
            while (token && exist_count < max_cols) {
                existing[exist_count] = (char*)malloc(strlen(token) + 1);
                if (existing[exist_count]) strcpy(existing[exist_count], token);
                exist_count++;
                token = strtok_r(NULL, ",", &saveptr);
            }
            free(copy);
        }
    }

    /* 释放旧行数据，立即置 NULL 防悬空 */
    free(in->rows[row_idx]);
    in->rows[row_idx] = NULL;

    /* 计算所需大小 */
    size_t total = 0;
    for (size_t i = 0; i < in->header_count; i++) {
        const char *col_val = ((int)i == col_idx) ? value :
            (i < exist_count && existing[i] ? existing[i] : "");
        total += strlen(col_val);
        if (i > 0) total++; /* comma */
    }

    in->rows[row_idx] = (char*)malloc(total + 1);
    if (!in->rows[row_idx]) {
        for (size_t i = 0; i < exist_count; i++) free(existing[i]);
        free(existing);
        return MSG_ERR_NO_MEMORY;
    }

    char *buf = in->rows[row_idx];
    size_t pos = 0;
    for (size_t i = 0; i < in->header_count; i++) {
        const char *col_val = ((int)i == col_idx) ? value :
            (i < exist_count && existing[i] ? existing[i] : "");
        size_t clen = strlen(col_val);
        if (i > 0) buf[pos++] = ',';
        memcpy(buf + pos, col_val, clen);
        pos += clen;
    }
    buf[pos] = '\0';

    for (size_t i = 0; i < exist_count; i++) free(existing[i]);
    free(existing);
    return 0;
}

int msg_set_value_i32(msg_packet_t *packet, const char *key, int32_t value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", value);
    return msg_set_value_str(packet, key, buf);
}

int msg_set_value_i64(msg_packet_t *packet, const char *key, int64_t value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)value);
    return msg_set_value_str(packet, key, buf);
}

int msg_set_value_double(msg_packet_t *packet, const char *key, double value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.15g", value);
    return msg_set_value_str(packet, key, buf);
}

int msg_clear_rows(msg_packet_t *packet) {
    if (!packet) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in) return MSG_ERR_NULL_PTR;

    for (size_t i = 0; i < in->row_count; i++) free(in->rows[i]);
    free(in->rows);
    in->rows = NULL;
    in->row_count = 0;
    return 0;
}

/* ============================================ */
/* 提交 */
/* ============================================ */

int msg_finalize(msg_packet_t *packet) {
    if (!packet) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in) return MSG_ERR_NULL_PTR;

    /* 自动设置时间戳（若为空或全零） */
    {
        bool need_ts = true;
        for (int i = 0; i < 17; i++) {
            if (packet->header.timestamp[i] != '0') { need_ts = false; break; }
        }
        if (need_ts) generate_timestamp_str(packet->header.timestamp);
    }

    /* 构建 unescaped body */
    size_t body_cap = 4096;
    uint8_t *unescaped = (uint8_t*)malloc(body_cap);
    if (!unescaped) return MSG_ERR_NO_MEMORY;
    size_t body_len = 0;

#define FINALIZE_ENSURE(need) do { \
    if (ensure_body_capacity(&unescaped, &body_cap, body_len + (need)) != 0) \
        { free(unescaped); return MSG_ERR_NO_MEMORY; } \
} while(0)

    /* 表头区: Field1[US]Field2[US]...FieldN[FS] */
    for (size_t i = 0; i < in->header_count; i++) {
        if (i > 0) {
            FINALIZE_ENSURE(1);
            unescaped[body_len++] = MSG_SEP_COL;
        }
        size_t hlen = strlen(in->headers[i]);
        FINALIZE_ENSURE(hlen);
        memcpy(unescaped + body_len, in->headers[i], hlen);
        body_len += hlen;
    }
    FINALIZE_ENSURE(1);
    unescaped[body_len++] = MSG_SEP_SECTION;

    /* 数据区: Row1[RS]Row2[RS]...RowN（最后行无 RS） */
    for (size_t r = 0; r < in->row_count; r++) {
        if (r > 0) {
            FINALIZE_ENSURE(1);
            unescaped[body_len++] = MSG_SEP_ROW;
        }
        if (in->rows[r]) {
            /* 行数据中的逗号替换为 US */
            char *row = in->rows[r];
            size_t rlen = strlen(row);
            FINALIZE_ENSURE(rlen);
            for (size_t k = 0; k < rlen; k++) {
                unescaped[body_len++] = (row[k] == ',') ? MSG_SEP_COL : (uint8_t)row[k];
            }
        }
    }

#undef FINALIZE_ENSURE

    /* 转义 */
    size_t escaped_len = 0;
    uint8_t *escaped = msg_escape(unescaped, body_len, &escaped_len);
    free(unescaped);
    if (!escaped) return MSG_ERR_NO_MEMORY;
    if (escaped_len > MSG_MAX_BODY_LEN) { free(escaped); return MSG_ERR_BODY_TOO_LARGE; }

    /* 构建 wire 缓冲区: magic[4] + crc32[4] + body_len[4] + header[HEAD_SIZE] + body[] */
    size_t wire_size = BODY_OFFSET + escaped_len;
    uint8_t *wire = (uint8_t*)malloc(wire_size);
    if (!wire) { free(escaped); return MSG_ERR_NO_MEMORY; }

    memcpy(wire, MSG_MAGIC, 4);
    memset(wire + 4, 0, 4);  /* crc32 placeholder */
    *(uint32_t*)(wire + 8) = MSG_HTOLE32((uint32_t)escaped_len);  /* body_len */

    /* 拷贝 header */
    memcpy(wire + 12, &packet->header, HEAD_SIZE);

    /* 拷贝转义后 body */
    if (escaped_len > 0) memcpy(wire + BODY_OFFSET, escaped, escaped_len);
    free(escaped);

    /* 一次性 CRC32（body_len + header + body，不含 magic 和 crc32） */
    uint32_t crc = crc32_update(0, wire + 8, 4 + HEAD_SIZE + escaped_len);
    *(uint32_t*)(wire + 4) = MSG_HTOLE32(crc);

    /* 更新 packet->body_len 为转义后长度（wire 格式） */
    packet->body_len = MSG_HTOLE32((uint32_t)escaped_len);

    /* 保存 */
    free(in->wire_buf);
    in->wire_buf = wire;
    in->wire_size = wire_size;
    in->finalized = true;
    in->cursor_row = (size_t)-1;

    return 0;
}

const void* msg_data(const msg_packet_t *packet) {
    if (!packet) return NULL;
    msg_internal_t *in = internal_get(packet);
    if (!in || !in->finalized) return NULL;
    return in->wire_buf;
}

size_t msg_size(const msg_packet_t *packet) {
    if (!packet) return 0;
    msg_internal_t *in = internal_get(packet);
    if (!in || !in->finalized) return 0;
    return in->wire_size;
}

/* ============================================ */
/* 编码/解码 */
/* ============================================ */

int msg_encode(const msg_packet_t *packet, void **out_buf, size_t *out_len) {
    if (!packet || !out_buf || !out_len) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in || !in->finalized || !in->wire_buf) return MSG_ERR_NOT_FINALIZED;

    *out_buf = malloc(in->wire_size);
    if (!*out_buf) return MSG_ERR_NO_MEMORY;

    memcpy(*out_buf, in->wire_buf, in->wire_size);
    *out_len = in->wire_size;
    return 0;
}

int msg_decode(const void *buf, size_t len, msg_packet_t **out_packet) {
    if (!buf || !out_packet) return MSG_ERR_NULL_PTR;
    if (len < BODY_OFFSET) return MSG_ERR_BUFFER_TOO_SMALL;

    const uint8_t *data = (const uint8_t*)buf;

    /* 检查 magic */
    if (memcmp(data, MSG_MAGIC, 4) != 0) return MSG_ERR_INVALID_MAGIC;

    /* 读取 body_len */
    uint32_t body_len = MSG_LE32TOH(*(uint32_t*)(data + BODY_LEN_POS));

    if (body_len > MSG_MAX_BODY_LEN) return MSG_ERR_BODY_TOO_LARGE;
    if (len < BODY_OFFSET + body_len) return MSG_ERR_BUFFER_TOO_SMALL;

    /* 验证 CRC（body_len + header + body，不含 magic 和 crc32） */
    uint32_t saved_crc = MSG_LE32TOH(*(uint32_t*)(data + 4));
    uint32_t calc_crc = crc32_update(0, data + 8, 4 + HEAD_SIZE + body_len);
    if (calc_crc != saved_crc) return MSG_ERR_CRC_MISMATCH;

    /* 分配 internal + packet */
    msg_internal_t *in = (msg_internal_t*)calloc(1, sizeof(msg_internal_t));
    if (!in) return MSG_ERR_NO_MEMORY;

    msg_packet_t *packet = packet_alloc(in);
    if (!packet) { free(in); return MSG_ERR_NO_MEMORY; }

    /* 复制 magic + crc32 + body_len + header（不触碰 body[]） */
    memcpy(packet, data, BODY_OFFSET);

    /* 验证 msg_type */
    if (!msg_is_valid_type(packet->header.msg_type)) {
        msg_destroy(packet);
        return MSG_ERR_INVALID_MSG_TYPE;
    }

    /* 转义还原 body */
    if (body_len > 0) {
        in->unescaped_body = msg_unescape(data + BODY_OFFSET, body_len, &in->unescaped_len);
        if (!in->unescaped_body) {
            msg_destroy(packet);
            return MSG_ERR_ESCAPE_SEQUENCE;
        }
        packet->body_len = MSG_HTOLE32((uint32_t)in->unescaped_len);
    }

    /* 解析 body */
    int rc = internal_parse_body(in);
    if (rc != 0) { msg_destroy(packet); return rc; }

    /* 保存 header 名称（从解析出的 header_fields 提取） */
    if (in->header_field_count > 0 && in->unescaped_body) {
        in->headers = (char**)calloc(in->header_field_count, sizeof(char*));
        if (in->headers) {
            in->header_count = in->header_field_count;
            for (size_t i = 0; i < in->header_field_count; i++) {
                size_t hlen = in->header_fields[i].len;
                in->headers[i] = (char*)malloc(hlen + 1);
                if (in->headers[i]) {
                    memcpy(in->headers[i],
                           in->unescaped_body + in->header_fields[i].offset, hlen);
                    in->headers[i][hlen] = '\0';
                }
            }
        }
    }

    /* 保存 wire buffer（用于 msg_encode 等） */
    in->wire_buf = (uint8_t*)malloc(len);
    if (!in->wire_buf) { msg_destroy(packet); return MSG_ERR_NO_MEMORY; }
    memcpy(in->wire_buf, data, len);
    in->wire_size = len;

    in->finalized = true;
    in->cursor_row = (size_t)-1;

    *out_packet = packet;
    return 0;
}

void msg_free_buffer(void *buf) {
    free(buf);
}

/* ============================================ */
/* 数据遍历 */
/* ============================================ */

bool msg_fetch_next(msg_packet_t *packet) {
    if (!packet) return false;
    msg_internal_t *in = internal_get(packet);
    if (!in || !in->finalized) return false;

    if (in->cursor_row == (size_t)-1) in->cursor_row = 0;
    else in->cursor_row++;

    return in->cursor_row < in->data_row_count;
}

void msg_reset_cursor(msg_packet_t *packet) {
    if (!packet) return;
    msg_internal_t *in = internal_get(packet);
    if (in) in->cursor_row = (size_t)-1;
}

size_t msg_get_current_row(const msg_packet_t *packet) {
    if (!packet) return 0;
    msg_internal_t *in = internal_get(packet);
    return in ? in->cursor_row : 0;
}

/* ============================================ */
/* 字段值获取（按 key，当前游标行） */
/* ============================================ */

int msg_get_value_str(msg_packet_t *packet, const char *key,
                      const char **out_val, size_t *out_len) {
    if (!packet || !key || !out_val || !out_len) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in || !in->unescaped_body) return MSG_ERR_NO_DATA;

    int col_idx = internal_find_col(in, key);
    if (col_idx < 0) { *out_val = NULL; *out_len = 0; return MSG_ERR_NO_DATA; }

    if (in->cursor_row >= in->data_row_count) {
        *out_val = NULL; *out_len = 0; return MSG_ERR_NO_DATA;
    }

    size_t f_start, f_len;
    if (!row_get_field_at(in->unescaped_body,
                          in->data_rows[in->cursor_row].offset,
                          in->data_rows[in->cursor_row].len,
                          (size_t)col_idx, &f_start, &f_len)) {
        *out_val = NULL; *out_len = 0;
        return MSG_ERR_NO_DATA;
    }

    *out_val = (const char*)(in->unescaped_body + f_start);
    *out_len = f_len;
    return 0;
}

int msg_get_value_i32(msg_packet_t *packet, const char *key, int32_t *out_val) {
    const char *val = NULL; size_t len = 0;
    int rc = msg_get_value_str(packet, key, &val, &len);
    if (rc == 0 && val && out_val) *out_val = (int32_t)atoi(val);
    return rc;
}

int msg_get_value_i64(msg_packet_t *packet, const char *key, int64_t *out_val) {
    const char *val = NULL; size_t len = 0;
    int rc = msg_get_value_str(packet, key, &val, &len);
    if (rc == 0 && val && out_val) *out_val = (int64_t)atoll(val);
    return rc;
}

int msg_get_value_double(msg_packet_t *packet, const char *key, double *out_val) {
    const char *val = NULL; size_t len = 0;
    int rc = msg_get_value_str(packet, key, &val, &len);
    if (rc == 0 && val && out_val) *out_val = atof(val);
    return rc;
}

/* ============================================ */
/* 字段值获取（按行列索引） */
/* ============================================ */

int msg_get_field(msg_packet_t *packet, size_t row, size_t col,
                  const char **out_val, size_t *out_len) {
    if (!packet || !out_val || !out_len) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in || !in->unescaped_body) return MSG_ERR_NO_DATA;
    if (row >= in->data_row_count) return MSG_ERR_NO_DATA;

    size_t f_start, f_len;
    if (!row_get_field_at(in->unescaped_body,
                          in->data_rows[row].offset,
                          in->data_rows[row].len,
                          col, &f_start, &f_len)) {
        *out_val = NULL; *out_len = 0;
        return MSG_ERR_NO_DATA;
    }

    *out_val = (const char*)(in->unescaped_body + f_start);
    *out_len = f_len;
    return 0;
}

/* ============================================ */
/* 统计 */
/* ============================================ */

size_t msg_get_header_count(const msg_packet_t *packet) {
    if (!packet) return 0;
    msg_internal_t *in = internal_get(packet);
    return in ? in->header_count : 0;
}

size_t msg_get_row_count(const msg_packet_t *packet) {
    if (!packet) return 0;
    msg_internal_t *in = internal_get(packet);
    return in ? in->data_row_count : 0;
}

/* ============================================ */
/* wire → 可读字符串 */
/* ============================================ */

/* 将 packet 的 wire 数据转为可读字符串，调用者需 msg_free_buffer 释放。
 * 分隔符替换为 <US>/<RS>/<FS>/<ESC>，不可打印字节替换为 '.'
 * 从 msg_id 开始，跳过 magic(4) + crc32(4)
 * 传入 NULL packet 或未 finalized 的 packet 返回 NULL */
char* msg_wire_to_string(const msg_packet_t *packet) {
    if (!packet) return NULL;
    const uint8_t *data = (const uint8_t*)msg_data(packet);
    size_t len = msg_size(packet);
    if (!data || len < 12) return NULL;

    /* 跳过 magic(4) + crc32(4) + body_len(4)，从 msg_id 开始 */
    data += 12;
    len  -= 12;

    /* 最坏情况：每个字节都变成 6 字符标记 "<ESC>"，+1 for \0 */
    size_t max_out = len * 6 + 1;
    char *out = (char*)malloc(max_out);
    if (!out) return NULL;

    char *wp = out;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        switch (c) {
        case 0x1F: memcpy(wp, "<US>", 4);  wp += 4; break;
        case 0x1E: memcpy(wp, "<RS>", 4);  wp += 4; break;
        case 0x1C: memcpy(wp, "<FS>", 4);  wp += 4; break;
        case 0x1B: memcpy(wp, "<ESC>", 5); wp += 5; break;
        default:
            *wp++ = isprint(c) ? (char)c : '#';
            break;
        }
    }
    *wp = '\0';
    return out;
}
