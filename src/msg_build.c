#include "msg_build.h"

/* encode_rs 已在 msg_api.c 中声明为 static，这里提供外部链接版本
 * 单遍编码：每次写入前检查容量，不够就扩容 */
int encode_rs(uint8_t *buf, size_t *cap, size_t *pos,
              char **headers, size_t header_count,
              char ***rows, size_t row_count, size_t col_count) {
    /* 表头区: Field1[US]Field2[US]...FieldN[FS] */
    for (size_t i = 0; i < header_count; i++) {
        if (i > 0) {
            if (ensure_body_capacity(&buf, cap, *pos + 1) != 0) return MSG_ERR_NO_MEMORY;
            buf[(*pos)++] = MSG_SEP_COL;
        }
        size_t hlen = strlen(headers[i]);
        if (ensure_body_capacity(&buf, cap, *pos + hlen) != 0) return MSG_ERR_NO_MEMORY;
        memcpy(buf + *pos, headers[i], hlen);
        *pos += hlen;
    }
    if (ensure_body_capacity(&buf, cap, *pos + 1) != 0) return MSG_ERR_NO_MEMORY;
    buf[(*pos)++] = MSG_SEP_SECTION;

    /* 数据区: Row1[RS]Row2[RS]...RowN（最后行无 RS） */
    for (size_t r = 0; r < row_count; r++) {
        if (r > 0) {
            if (ensure_body_capacity(&buf, cap, *pos + 1) != 0) return MSG_ERR_NO_MEMORY;
            buf[(*pos)++] = MSG_SEP_ROW;
        }
        for (size_t c = 0; c < col_count; c++) {
            if (c > 0) {
                if (ensure_body_capacity(&buf, cap, *pos + 1) != 0) return MSG_ERR_NO_MEMORY;
                buf[(*pos)++] = MSG_SEP_COL;
            }
            char *val = rows[r][c];
            if (val) {
                size_t vlen = strlen(val);
                if (ensure_body_capacity(&buf, cap, *pos + vlen) != 0) return MSG_ERR_NO_MEMORY;
                memcpy(buf + *pos, val, vlen);
                *pos += vlen;
            }
        }
    }
    return 0;
}

int ensure_body_capacity(uint8_t **buf, size_t *cap, size_t needed) {
    while (needed >= *cap) {
        size_t new_cap = *cap * 2;
        uint8_t *tmp = (uint8_t*)realloc(*buf, new_cap);
        if (!tmp) return MSG_ERR_NO_MEMORY;
        *buf = tmp;
        *cap = new_cap;
    }
    return 0;
}
