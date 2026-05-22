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

#include "msg_internal.h"
#include "msg_build.h"
#include "msg_query.h"

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
        version ? version : MSG_VERSION_DEFAULT, HEAD_VER_LENGTH);

    packet->header.format = MSG_FORMAT_TABLE;
    packet->header.msg_type = msg_type;
    generate_timestamp_str(packet->header.timestamp);
    msg_copy_fixed_field(packet->header.func, "", HEAD_FUNC_LENGTH);

    /* 初始化多结果集：默认 RS0（第一结果集） */
    in->current_rs = 0;
    in->rs_count = 1;
    in->rs_cap = 4;
    in->result_sets = (result_set_t*)calloc(4, sizeof(result_set_t));
    if (!in->result_sets) {
        free(in);
        free((char*)packet - sizeof(void*));
        return NULL;
    }
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
    new_in->rs_count = old_in->rs_count;
    new_in->current_rs = old_in->current_rs;
    new_in->cursor_row = old_in->cursor_row;

    /* 复制 result_sets 数组 */
    if (old_in->rs_count > 0) {
        if (ensure_rs_cap(new_in, old_in->rs_count) != 0) goto clone_fail;
        for (size_t i = 0; i < old_in->rs_count; i++) {
            result_set_t *src = &old_in->result_sets[i];
            result_set_t *dst = &new_in->result_sets[i];

            /* 复制 headers */
            if (src->header_count > 0) {
                dst->headers = (char**)malloc(sizeof(char*) * src->header_count);
                if (!dst->headers) goto clone_fail;
                dst->header_count = src->header_count;
                for (size_t j = 0; j < src->header_count; j++) {
                    dst->headers[j] = (char*)malloc(strlen(src->headers[j]) + 1);
                    if (!dst->headers[j]) goto clone_fail;
                    strcpy(dst->headers[j], src->headers[j]);
                }
            }

            /* 复制 rows：每个单元格独立 malloc，与 packet_free_internal 释放逻辑一致
             * 布局: rows[j][k] = strdup(src->rows[j][k]) */
            if (src->row_count > 0 && src->header_count > 0) {
                dst->rows = (char***)malloc(sizeof(char**) * src->row_count);
                if (!dst->rows) goto clone_fail;
                for (size_t j = 0; j < src->row_count; j++) {
                    dst->rows[j] = (char**)malloc(sizeof(char*) * src->header_count);
                    if (!dst->rows[j]) {
                        for (size_t k = 0; k < j; k++) free(dst->rows[k]);
                        free(dst->rows);
                        dst->rows = NULL;
                        goto clone_fail;
                    }
                    for (size_t k = 0; k < src->header_count; k++) {
                        if (src->rows[j][k]) {
                            dst->rows[j][k] = strdup(src->rows[j][k]);
                            if (!dst->rows[j][k]) {
                                for (size_t m = 0; m < k; m++) free(dst->rows[j][m]);
                                for (size_t m = 0; m < j; m++) {
                                    for (size_t n = 0; n < src->header_count; n++) free(dst->rows[m][n]);
                                    free(dst->rows[m]);
                                }
                                free(dst->rows[j]);
                                free(dst->rows);
                                dst->rows = NULL;
                                goto clone_fail;
                            }
                        } else {
                            dst->rows[j][k] = NULL;
                        }
                    }
                }
                dst->row_count = src->row_count;
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

    /* unescaped body */
    if (old_in->unescaped_body && old_in->unescaped_len > 0) {
        new_in->unescaped_body = (uint8_t*)malloc(old_in->unescaped_len);
        if (!new_in->unescaped_body) goto clone_fail;
        memcpy(new_in->unescaped_body, old_in->unescaped_body, old_in->unescaped_len);
        new_in->unescaped_len = old_in->unescaped_len;
    }

    return clone;

clone_fail:
    /* 释放已部分分配的 result_sets */
    for (size_t i = 0; i < new_in->rs_count; i++) {
        result_set_t *rs = &new_in->result_sets[i];
        if (rs->headers) {
            for (size_t j = 0; j < rs->header_count; j++) free(rs->headers[j]);
            free(rs->headers);
        }
        if (rs->rows) {
            for (size_t j = 0; j < rs->row_count; j++) {
                if (rs->rows[j]) {
                    for (size_t k = 0; k < rs->header_count; k++) free(rs->rows[j][k]);
                    free(rs->rows[j]);
                }
            }
            free(rs->rows);
        }
    }
    free(new_in->result_sets);
    free(new_in->wire_buf);
    free(new_in->unescaped_body);
    free(new_in);
    free((char*)clone - sizeof(void*));
    return NULL;
}

/* ============================================ */
/* Header 字段设置 */
/* ============================================ */

int msg_set_msg_id(msg_packet_t *packet, const char *msg_id) {
    if (!packet || !msg_id) return MSG_ERR_NULL_PTR;
    msg_copy_fixed_field(packet->header.msg_id, msg_id, HEAD_MSGID_LENGTH);
    return 0;
}

int msg_set_func(msg_packet_t *packet, const char *func) {
    if (!packet || !func) return MSG_ERR_NULL_PTR;
    msg_copy_fixed_field(packet->header.func, func, HEAD_FUNC_LENGTH);
    return 0;
}

int msg_set_type(msg_packet_t *packet, uint8_t msg_type) {
    if (!packet) return MSG_ERR_NULL_PTR;
    if (!msg_is_valid_type(msg_type)) return MSG_ERR_INVALID_MSG_TYPE;
    packet->header.msg_type = msg_type;
    return 0;
}

int msg_set_timestamp(msg_packet_t *packet, const char *timestamp) {
    if (!packet) return MSG_ERR_NULL_PTR;
    if (!timestamp || timestamp[0] == '\0') {
        generate_timestamp_str(packet->header.timestamp);
    } else {
        memcpy(packet->header.timestamp, timestamp, 17);
        packet->header.timestamp[17] = '\0';  /* 确保 \0 终止 */
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
    msg_copy_fixed_field(packet->header.ver, version, HEAD_VER_LENGTH);
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

const char* msg_get_timestamp(const msg_packet_t *packet) {
    /* timestamp 是 char[17] 无\0，按长度读取，不依赖strlen */
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

    result_set_t *rs = current_rs_build(in);
    if (!rs) return MSG_ERR_NULL_PTR;

    /* 清除旧表头 */
    for (size_t i = 0; i < rs->header_count; i++) free(rs->headers[i]);
    free(rs->headers);
    rs->headers = NULL;
    rs->header_count = 0;

    rs->headers = (char**)calloc((size_t)column_count, sizeof(char*));
    if (!rs->headers) return MSG_ERR_NO_MEMORY;

    /* 解析逗号分隔的表头 */
    char *copy = (char*)malloc(strlen(headers) + 1);
    if (!copy) {
        free(rs->headers);
        rs->headers = NULL;
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
        rs->headers[idx] = (char*)malloc(tlen + 1);
        if (!rs->headers[idx]) { ret = MSG_ERR_NO_MEMORY; goto cleanup; }
        strcpy(rs->headers[idx], token);
        idx++;
        token = strtok_r(NULL, ",", &saveptr);
    }
    rs->header_count = idx;

    /* 验证解析出的表头数量与预期一致 */
    if (idx != (size_t)column_count) { ret = MSG_ERR_INVALID_FORMAT; goto cleanup; }

cleanup:
    free(copy);
    if (ret != 0) {
        for (size_t i = 0; i < idx; i++) free(rs->headers[i]);
        free(rs->headers);
        rs->headers = NULL;
        rs->header_count = 0;
    }
    return ret;
}

int msg_add_header(msg_packet_t *packet, const char *header) {
    if (!packet || !header) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in) return MSG_ERR_NULL_PTR;

    result_set_t *rs = current_rs_build(in);
    if (!rs) return MSG_ERR_NULL_PTR;
    if (rs->header_count >= MSG_MAX_HEADERS) return MSG_ERR_TOO_MANY_HEADERS;

    size_t hlen = strlen(header);
    if (hlen > MSG_MAX_FIELD_LEN) return MSG_ERR_FIELD_TOO_LONG;

    char *header_copy = (char*)malloc(hlen + 1);
    if (!header_copy) return MSG_ERR_NO_MEMORY;
    strcpy(header_copy, header);

    /* 已有行时，先扩展所有行的列数组，失败时 header_copy 泄漏但不影响状态 */
    if (rs->row_count > 0) {
        for (size_t i = 0; i < rs->row_count; i++) {
            char **new_cols = (char**)realloc(rs->rows[i], sizeof(char*) * (rs->header_count + 1));
            if (!new_cols) {
                free(header_copy);
                return MSG_ERR_NO_MEMORY;
            }
            new_cols[rs->header_count] = NULL;
            rs->rows[i] = new_cols;
        }
    }

    /* 扩展 headers 数组 */
    char **new_h = (char**)realloc(rs->headers, sizeof(char*) * (rs->header_count + 1));
    if (!new_h) {
        free(header_copy);
        return MSG_ERR_NO_MEMORY;
    }
    rs->headers = new_h;
    rs->headers[rs->header_count] = header_copy;
    rs->header_count++;
    return 0;
}

int msg_get_headers(const msg_packet_t *packet, char *out, size_t *out_len) {
    if (!packet || !out || !out_len) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in) return MSG_ERR_NULL_PTR;

    result_set_t *rs = current_rs_build(in);
    if (!rs) return MSG_ERR_NULL_PTR;

    /* 从内部 header 数组重建逗号分隔字符串 */
    size_t needed = 0;
    for (size_t i = 0; i < rs->header_count; i++) {
        needed += strlen(rs->headers[i]);
        if (i > 0) needed++; /* comma */
    }
    if (needed >= *out_len) { *out_len = needed + 1; return MSG_ERR_BUFFER_TOO_SMALL; }

    size_t pos = 0;
    for (size_t i = 0; i < rs->header_count; i++) {
        if (i > 0) out[pos++] = ',';
        size_t hlen = strlen(rs->headers[i]);
        memcpy(out + pos, rs->headers[i], hlen);
        pos += hlen;
    }
    out[pos] = '\0';
    *out_len = pos;
    return 0;
}

/* ============================================ */
/* 数据行构建 */
/* ============================================ */

int msg_add_row(msg_packet_t *packet) {
    if (!packet) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in) return MSG_ERR_NULL_PTR;

    result_set_t *rs = current_rs_build(in);
    if (!rs) return MSG_ERR_NULL_PTR;
    if (rs->row_count >= MSG_MAX_ROWS) return MSG_ERR_TOO_MANY_ROWS;

    char ***new_rows = (char***)realloc(rs->rows, sizeof(char**) * (rs->row_count + 1));
    if (!new_rows) return MSG_ERR_NO_MEMORY;
    rs->rows = new_rows;
    rs->rows[rs->row_count] = (char**)calloc(rs->header_count, sizeof(char*));
    if (!rs->rows[rs->row_count]) return MSG_ERR_NO_MEMORY;
    rs->row_count++;
    return 0;
}

int msg_set_row(msg_packet_t *packet, const char *fmt, ...) {
    if (!packet || !fmt) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in) return MSG_ERR_NULL_PTR;

    result_set_t *rs = current_rs_build(in);
    if (!rs || rs->row_count == 0) return MSG_ERR_NO_DATA;
    if (rs->header_count == 0) return MSG_ERR_INVALID_FORMAT;

    size_t row_idx = rs->row_count - 1;

    /* 先分配临时行，失败时不影响原数据 */
    char **tmp_row = (char**)malloc(sizeof(char*) * rs->header_count);
    if (!tmp_row) return MSG_ERR_NO_MEMORY;
    for (size_t c = 0; c < rs->header_count; c++) tmp_row[c] = NULL;

    /* 直接从 va_list 取每个参数，无需 vsnprintf 中转
     * header_count 为 size_t，不可能为 0（上面已检查），不会发生 unsigned underflow */
    va_list args;
    va_start(args, fmt);
    for (size_t col = 0; col < rs->header_count; col++) {
        const char *val = va_arg(args, const char *);
        if (!val) val = "";
        size_t vlen = strlen(val);
        if (vlen > MSG_MAX_FIELD_LEN) {
            va_end(args);
            for (size_t k = 0; k < col; k++) free(tmp_row[k]);
            free(tmp_row);
            return MSG_ERR_FIELD_TOO_LONG;
        }
        tmp_row[col] = (char*)malloc(vlen + 1);
        if (!tmp_row[col]) {
            va_end(args);
            for (size_t k = 0; k < col; k++) free(tmp_row[k]);
            free(tmp_row);
            return MSG_ERR_NO_MEMORY;
        }
        memcpy(tmp_row[col], val, vlen);
        tmp_row[col][vlen] = '\0';
    }
    va_end(args);

    /* 全部成功，替换旧行 */
    for (size_t c = 0; c < rs->header_count; c++) free(rs->rows[row_idx][c]);
    free(rs->rows[row_idx]);
    rs->rows[row_idx] = tmp_row;
    return 0;
}

int msg_set_value_str(msg_packet_t *packet, const char *key, const char *value) {
    if (!packet || !key || !value) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in) return MSG_ERR_NULL_PTR;

    result_set_t *rs = current_rs_build(in);
    if (!rs) return MSG_ERR_NULL_PTR;

    int col_idx = internal_find_build_col(in, key);
    if (col_idx < 0) {
        int rc = msg_add_header(packet, key);
        if (rc != 0) return rc;
        col_idx = (int)rs->header_count - 1;
    }

    if (strlen(value) > MSG_MAX_FIELD_LEN) return MSG_ERR_FIELD_TOO_LONG;

    /* 没有行时返回错误 */
    if (rs->row_count == 0) return MSG_ERR_NO_DATA;

    size_t row_idx = rs->row_count - 1;

    /* 直接按索引赋值，无需解析整行 */
    free(rs->rows[row_idx][col_idx]);
    char *new_val = strdup(value);
    if (!new_val) return MSG_ERR_NO_MEMORY;
    rs->rows[row_idx][col_idx] = new_val;

    return 0;
}

int msg_set_value_i32(msg_packet_t *packet, const char *key, int32_t value) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    return msg_set_value_str(packet, key, buf);
}

int msg_set_value_i64(msg_packet_t *packet, const char *key, int64_t value) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%lld", (long long)value);
    return msg_set_value_str(packet, key, buf);
}

int msg_set_value_double(msg_packet_t *packet, const char *key, double value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.15g", value);
    return msg_set_value_str(packet, key, buf);
}

int msg_clear_rows(msg_packet_t *packet) {
    if (!packet) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in) return MSG_ERR_NULL_PTR;

    result_set_t *rs = current_rs_build(in);
    if (!rs) return MSG_ERR_NULL_PTR;

    for (size_t i = 0; i < rs->row_count; i++) {
        for (size_t c = 0; c < rs->header_count; c++) {
            free(rs->rows[i][c]);
        }
        free(rs->rows[i]);
    }
    free(rs->rows);
    rs->rows = NULL;
    rs->row_count = 0;
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
        for (int i = 0; i < HEAD_TIMESTAMP_LENGTH; i++) {
            if (packet->header.timestamp[i] != '0') { need_ts = false; break; }
        }
        if (need_ts) generate_timestamp_str(packet->header.timestamp);
    }

    /* 构建 unescaped body */
    size_t body_cap = 4096;
    uint8_t *unescaped = (uint8_t*)malloc(body_cap);
    if (!unescaped) return MSG_ERR_NO_MEMORY;
    size_t body_len = 0;

    /* 编码每个结果集 */
    for (size_t ri = 0; ri < in->rs_count; ri++) {
        result_set_t *rs = &in->result_sets[ri];
        int rc = encode_rs(unescaped, &body_cap, &body_len,
                           rs->headers, rs->header_count,
                           rs->rows, rs->row_count, rs->header_count);

        if (rc != 0) { free(unescaped); return rc; }

        /* 结果集之间加 GS 分隔（最后一个不加） */
        if (ri < in->rs_count - 1) {
            if (ensure_body_capacity(&unescaped, &body_cap, body_len + 1) != 0) {
                free(unescaped); return MSG_ERR_NO_MEMORY;
            }
            unescaped[body_len++] = MSG_SEP_RS_GROUP;
        }
    }

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
    memset(wire + 4, 0, MSG_CRC32_SIZE);  /* crc32 placeholder */
    *(uint32_t*)(wire + 8) = MSG_HTOLE32((uint32_t)escaped_len);  /* body_len */

    /* 拷贝 header */
    memcpy(wire + MSG_PRE_HEADER_SIZE, &packet->header, HEAD_SIZE);

    /* 拷贝转义后 body */
    if (escaped_len > 0) memcpy(wire + BODY_OFFSET, escaped, escaped_len);
    free(escaped);

    /* 一次性 CRC32（body_len + header + body，不含 magic 和 crc32） */
    uint32_t crc = crc32_update(0, wire + 8, MSG_BODY_LEN_SIZE + HEAD_SIZE + escaped_len);
    *(uint32_t*)(wire + 4) = MSG_HTOLE32(crc);

    /* 更新 packet->body_len 为转义后长度（wire 格式） */
    packet->body_len = MSG_HTOLE32((uint32_t)escaped_len);

    /* 保存 */
    free(in->wire_buf);
    in->wire_buf = wire;
    in->wire_size = wire_size;
    in->cursor_row = (size_t)-1;

    return 0;
}

const void* msg_data(const msg_packet_t *packet) {
    if (!packet) return NULL;
    msg_internal_t *in = internal_get(packet);
    if (!in || !in->wire_buf) return NULL;
    return in->wire_buf;
}

size_t msg_size(const msg_packet_t *packet) {
    if (!packet) return 0;
    msg_internal_t *in = internal_get(packet);
    if (!in || !in->wire_buf) return 0;
    return in->wire_size;
}

/* ============================================ */
/* 编码/解码 */
/* ============================================ */

int msg_encode(const msg_packet_t *packet, void **out_buf, size_t *out_len) {
    if (!packet || !out_buf || !out_len) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in || !in->wire_buf) return MSG_ERR_NOT_FINALIZED;

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
    uint32_t calc_crc = crc32_update(0, data + 8, MSG_BODY_LEN_SIZE + HEAD_SIZE + body_len);
    if (calc_crc != saved_crc) return MSG_ERR_CRC_MISMATCH;

    /* 分配 internal + packet */
    msg_internal_t *in = (msg_internal_t*)calloc(1, sizeof(msg_internal_t));
    if (!in) return MSG_ERR_NO_MEMORY;

    msg_packet_t *packet = packet_alloc(in);
    if (!packet) { free(in); return MSG_ERR_NO_MEMORY; }

    /* 复制 magic[4] + crc32[4] + body_len[4] + header[HEAD_SIZE] */
    memcpy(packet, data, MSG_PRE_HEADER_SIZE + HEAD_SIZE);

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

    /* 解析 body（headers 和 rows 直接由 parse_single_rs 构建） */
    int rc = internal_parse_body(in);
    if (rc != 0) { msg_destroy(packet); return rc; }

    /* 保存 wire buffer（用于 msg_encode 等） */
    in->wire_buf = (uint8_t*)malloc(len);
    if (!in->wire_buf) { msg_destroy(packet); return MSG_ERR_NO_MEMORY; }
    memcpy(in->wire_buf, data, len);
    in->wire_size = len;

    in->cursor_row = (size_t)-1;
    in->current_rs = 0;  /* 默认指向 RS0 */

    *out_packet = packet;
    return 0;
}

void msg_free_buffer(void *buf) {
    free(buf);
}

/* ============================================ */
/* 数据遍历（当前结果集） */
/* ============================================ */

bool msg_fetch_next(msg_packet_t *packet) {
    if (!packet) return false;
    msg_internal_t *in = internal_get(packet);
    if (!in) return false;

    result_set_t *rs = current_rs_build(in);
    size_t row_count = rs ? rs->row_count : 0;

    /* 首次调用(-1)：置0；后续调用：推进 */
    if (in->cursor_row == (size_t)-1) in->cursor_row = 0;
    else in->cursor_row++;

    return in->cursor_row < row_count;
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

/* 前置声明（字段值获取辅助） */
static int msg_get_field_impl(msg_internal_t *in, size_t row, size_t col,
                              const char **out_val, size_t *out_len);

/* ============================================ */
/* 字段值获取（按 key，当前游标行） */
/* ============================================ */

int msg_get_value_str(msg_packet_t *packet, const char *key,
                      const char **out_val, size_t *out_len) {
    if (!packet || !key || !out_val || !out_len) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in || !in->unescaped_body) return MSG_ERR_NO_DATA;

    result_set_t *rs = current_rs_build(in);
    if (!rs || rs->row_count == 0) return MSG_ERR_NO_DATA;

    /* cursor 未初始化时，自动推进到第一行（兼容从未调用 msg_fetch_next 的场景） */
    if (in->cursor_row == (size_t)-1) in->cursor_row = 0;
    if (in->cursor_row >= rs->row_count) return MSG_ERR_NO_DATA;

    int col_idx = internal_find_col(in, key);
    if (col_idx < 0) { *out_val = NULL; *out_len = 0; return MSG_ERR_NO_DATA; }

    return msg_get_field_impl(in, in->cursor_row, (size_t)col_idx, out_val, out_len);
}

/* 数值类型转换辅助（避免重复代码） */
static int msg_get_value_as(msg_packet_t *packet, const char *key,
                           char *buf, size_t buf_size,
                           void (*conv)(char *, size_t, void*), void *val) {
    const char *v = NULL; size_t len = 0;
    int rc = msg_get_value_str(packet, key, &v, &len);
    if (rc != 0 || !v) return rc;
    memcpy(buf, v, len < buf_size - 1 ? len : buf_size - 1);
    buf[len < buf_size - 1 ? len : buf_size - 1] = '\0';
    conv(buf, buf_size, val);
    return 0;
}

static void conv_i32(char *s, size_t n, void *val) { (void)n; *(int32_t*)val = (int32_t)atoi(s); }
static void conv_i64(char *s, size_t n, void *val) { (void)n; *(int64_t*)val = atoll(s); }
static void conv_double(char *s, size_t n, void *val) { (void)n; *(double*)val = atof(s); }

int msg_get_value_i32(msg_packet_t *packet, const char *key, int32_t *out_val) {
    char buf[32];
    return msg_get_value_as(packet, key, buf, sizeof(buf), conv_i32, out_val);
}

int msg_get_value_i64(msg_packet_t *packet, const char *key, int64_t *out_val) {
    char buf[32];
    return msg_get_value_as(packet, key, buf, sizeof(buf), conv_i64, out_val);
}

int msg_get_value_double(msg_packet_t *packet, const char *key, double *out_val) {
    char buf[64];
    return msg_get_value_as(packet, key, buf, sizeof(buf), conv_double, out_val);
}

/* ============================================ */
/* 字段值获取（按行列索引） */
/* ============================================ */

static int msg_get_field_impl(msg_internal_t *in, size_t row, size_t col,
                              const char **out_val, size_t *out_len) {
    result_set_t *rs = current_rs_build(in);
    if (!rs) return MSG_ERR_NO_DATA;
    if (row >= rs->row_count || !rs->rows) return MSG_ERR_NO_DATA;
    if (col >= rs->header_count) return MSG_ERR_NO_DATA;
    *out_val = rs->rows[row][col];
    *out_len = strlen(rs->rows[row][col]);
    return 0;
}

int msg_get_field(msg_packet_t *packet, size_t row, size_t col,
                  const char **out_val, size_t *out_len) {
    if (!packet || !out_val || !out_len) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in || !in->unescaped_body) return MSG_ERR_NO_DATA;
    return msg_get_field_impl(in, row, col, out_val, out_len);
}

/* ============================================ */
/* 统计 */
/* ============================================ */

size_t msg_get_header_count(const msg_packet_t *packet) {
    if (!packet) return 0;
    msg_internal_t *in = internal_get(packet);
    if (!in) return 0;

    result_set_t *rs = current_rs_build(in);
    return rs ? rs->header_count : 0;
}

size_t msg_get_row_count(const msg_packet_t *packet) {
    if (!packet) return 0;
    msg_internal_t *in = internal_get(packet);
    if (!in) return 0;

    result_set_t *rs = current_rs_build(in);
    return rs ? rs->row_count : 0;
}

/* ============================================ */
/* 多结果集支持 */
/* ============================================ */

size_t msg_get_result_set(const msg_packet_t *packet) {
    if (!packet) return 0;
    msg_internal_t *in = internal_get(packet);
    if (!in) return 0;
    return in->current_rs + 1;  /* 1-based */
}

bool msg_add_result_set(msg_packet_t *packet) {
    if (!packet) return false;
    msg_internal_t *in = internal_get(packet);
    if (!in) return false;

    /* 扩容并初始化新结果集 */
    size_t next_rs = in->rs_count;
    if (next_rs >= in->rs_cap) {
        if (ensure_rs_cap(in, next_rs + 1) != 0) return false;
    }

    in->rs_count = next_rs + 1;
    in->current_rs = next_rs;
    in->cursor_row = (size_t)-1;
    return true;
}

bool msg_next_result_set(msg_packet_t *packet) {
    if (!packet) return false;
    msg_internal_t *in = internal_get(packet);
    if (!in) return false;

    /* 已在最后一个结果集，无法继续 */
    if (in->current_rs >= in->rs_count - 1) return false;

    in->current_rs++;
    in->cursor_row = (size_t)-1;
    return true;
}

int msg_select_result_set(msg_packet_t *packet, size_t rs_number) {
    if (!packet) return MSG_ERR_NULL_PTR;
    msg_internal_t *in = internal_get(packet);
    if (!in) return MSG_ERR_NULL_PTR;

    if (rs_number < 1) return MSG_ERR_INVALID_FORMAT;

    size_t rs_idx = rs_number - 1;  /* 转为 0-based */

    /* 只允许选择已存在的结果集，不支持跳号创建
     * 原因：跳号会导致中间 result_sets 未初始化，访问时会读取未定义内存
     * 正确用法：先用 msg_add_result_set 创建所有需要的结果集，再调用本函数 */
    if (rs_idx >= in->rs_count) return MSG_ERR_INVALID_FORMAT;
    in->current_rs = rs_idx;
    in->cursor_row = (size_t)-1;
    return 0;
}

size_t msg_get_result_set_count(const msg_packet_t *packet) {
    if (!packet) return 0;
    msg_internal_t *in = internal_get(packet);
    if (!in) return 0;
    return in->rs_count > 0 ? in->rs_count : 1;
}

/* ============================================ */
/* wire → 可读字符串 */
/* ============================================ */

/* 将 packet 的 wire 数据转为可读字符串，调用者需 msg_free_buffer 释放。
 * 分隔符替换为 <US>/<RS>/<FS>/<ESC>，不可打印字节替换为 '.'
 * 从 msg_id 开始，跳过 magic(4) + crc32(4)
 * 传入 NULL packet 或 wire_buf 为空（未 finalize）返回 NULL */
char* msg_wire_to_string(const msg_packet_t *packet) {
    if (!packet) return NULL;

    msg_internal_t *in = internal_get(packet);
    if (!in || !in->wire_buf) return NULL;

    /* 始终从 wire_buf 读取（无论是构建的还是解码的），
     * 因为 wire_buf 始终保存原始线上字节（转义状态）。 */
    size_t wire_size = in->wire_size;
    if (wire_size < 12) return NULL;

    uint8_t *wire_buf = in->wire_buf;
    /* 从 msg_id 开始，跳过 magic(4) + crc32(4)，包含 header + body */
    size_t data_offset = 12;
    size_t data_len = (wire_size > data_offset) ? wire_size - data_offset : 0;

    /* 最坏情况：每个字节都变成 6 字符标记 "<ESC>"，+1 for \0 */
    size_t max_out = data_len * 6 + 1;
    char *out = (char*)malloc(max_out);
    if (!out) return NULL;

    char *wp = out;

    /* 写入从 msg_id 开始的全部内容（header + body，转义状态） */
    for (size_t i = 0; i < data_len; i++) {
        uint8_t c = wire_buf[data_offset + i];
        switch (c) {
        case 0x1F: memcpy(wp, "<US>", 4);  wp += 4; break;
        case 0x1E: memcpy(wp, "<RS>", 4);  wp += 4; break;
        case 0x1C: memcpy(wp, "<FS>", 4);  wp += 4; break;
        case 0x1D: memcpy(wp, "<GS>", 4);  wp += 4; break;
        case 0x1B: memcpy(wp, "<ESC>", 5); wp += 5; break;
        default:
            *wp++ = isprint(c) ? (char)c : '#';
            break;
        }
    }

    *wp = '\0';
    return out;
}
