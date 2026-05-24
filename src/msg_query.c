#include "msg_query.h"
#include "msg_internal.h"

/*
 * 解析单个结果集（从 start_offset 开始，到 end_offset 或 GS 之前结束）
 *
 * 两遍扫描：
 *   第一遍：计数 headers 和 rows/cols，避免反复 realloc
 *   第二遍：实际复制数据
 */
int parse_single_rs(void *in_ctx,
                    size_t start_offset, size_t end_offset,
                    char ***out_headers, size_t *out_hcnt,
                    char ****out_rows, size_t *out_rcnt) {
    msg_internal_t *in = (msg_internal_t*)in_ctx;
    char ***rows = NULL;
    size_t rcnt = 0;

    /* --------------------------------------------------
     * 第一遍：计数 headers（找 FS 确定列数）
     * -------------------------------------------------- */
    size_t h_start = start_offset;
    bool found_fs = false;
    size_t fs_pos = 0;
    size_t hcnt = 0;

    for (size_t i = start_offset; i < end_offset; i++) {
        uint8_t ch = in->unescaped_body[i];
        if (ch == MSG_SEP_SECTION || ch == MSG_SEP_COL) {
            hcnt++;
            if (ch == MSG_SEP_SECTION) { found_fs = true; fs_pos = i; break; }
            h_start = i + 1;
        }
    }

    /* --------------------------------------------------
     * 场景A：没有表头区，整个范围当数据行（RS1 可能只有数据无表头）
     * -------------------------------------------------- */
    if (!found_fs) {
        hcnt = 0;
        size_t len = end_offset - start_offset;
        rows = (char***)malloc(sizeof(char**) * 1);
        if (!rows) goto fail;
        rows[0] = (char**)calloc(1, sizeof(char*));
        if (!rows[0]) { free(rows); rows = NULL; goto fail; }
        rows[0][0] = (char*)malloc(len + 1);
        if (!rows[0][0]) { free(rows[0]); free(rows); rows = NULL; goto fail; }
        memcpy(rows[0][0], in->unescaped_body + start_offset, len);
        rows[0][0][len] = '\0';
        rcnt = 1;
        *out_headers = NULL; *out_hcnt = 0;
        *out_rows = rows; *out_rcnt = rcnt;
        return 0;
    }

    /* 分配 headers 数组（一次分配） */
    char **hdr = (char**)malloc(sizeof(char*) * hcnt);
    if (!hdr) return MSG_ERR_NO_MEMORY;

    /* --------------------------------------------------
     * 第二遍：复制 headers 数据
     * -------------------------------------------------- */
    hcnt = 0;
    h_start = start_offset;
    for (size_t i = start_offset; i < fs_pos; i++) {
        uint8_t ch = in->unescaped_body[i];
        if (ch == MSG_SEP_SECTION || ch == MSG_SEP_COL) {
            size_t len = i - h_start;
            hdr[hcnt] = (char*)malloc(len + 1);
            if (!hdr[hcnt]) { for (size_t k = 0; k < hcnt; k++) free(hdr[k]); free(hdr); goto fail; }
            memcpy(hdr[hcnt], in->unescaped_body + h_start, len);
            hdr[hcnt][len] = '\0';
            hcnt++;
            h_start = i + 1;
        }
    }
    /* 处理 header 区尾部（最后一个分隔符之后到 FS 之间的内容） */
    if (h_start < fs_pos) {
        size_t len = fs_pos - h_start;
        hdr[hcnt] = (char*)malloc(len + 1);
        if (!hdr[hcnt]) { for (size_t k = 0; k < hcnt; k++) free(hdr[k]); free(hdr); goto fail; }
        memcpy(hdr[hcnt], in->unescaped_body + h_start, len);
        hdr[hcnt][len] = '\0';
        hcnt++;
    }

    /* --------------------------------------------------
     * 第一遍（数据区）：计数 rows 和 cols
     * -------------------------------------------------- */
    size_t d_start = fs_pos + 1;
    rcnt = 0;
    size_t max_cols = 0;
    size_t col_idx = 0;

    for (size_t i = fs_pos + 1; i <= end_offset; i++) {
        if (i == end_offset || in->unescaped_body[i] == MSG_SEP_ROW) {
            rcnt++;
            if (col_idx > max_cols) max_cols = col_idx;
            col_idx = 0;
            d_start = i + 1;
        } else if (in->unescaped_body[i] == MSG_SEP_COL) {
            col_idx++;
        }
    }

    /* 分配 rows 数组（行指针数组 + 每行字段指针数组，一次分配） */
    rows = (char***)malloc(sizeof(char**) * rcnt);
    if (!rows) { for (size_t k = 0; k < hcnt; k++) free(hdr[k]); free(hdr); goto fail; }
    for (size_t r = 0; r < rcnt; r++) {
        rows[r] = (char**)calloc(max_cols + 1, sizeof(char*));
        if (!rows[r]) { for (size_t k = 0; k < r; k++) free(rows[k]); free(rows); goto fail; }
    }

    /* --------------------------------------------------
     * 第二遍（数据区）：复制字段值
     * 注意：f_start < end_offset 守卫防止空字段泄漏（RS3 空结果集场景）
     * -------------------------------------------------- */
    size_t cur_row = 0;
    size_t cur_col = 0;
    d_start = fs_pos + 1;
    size_t f_start = d_start;

    for (size_t i = fs_pos + 1; i <= end_offset; i++) {
        if (f_start < end_offset && (i == end_offset || in->unescaped_body[i] == MSG_SEP_ROW)) {
            size_t len = i - f_start;
            rows[cur_row][cur_col] = (char*)malloc(len + 1);
            if (!rows[cur_row][cur_col]) { for (size_t k = 0; k < cur_row; k++) { for (size_t m = 0; rows[k][m]; m++) free(rows[k][m]); free(rows[k]); } for (size_t k = 0; k < hcnt; k++) free(hdr[k]); free(hdr); free(rows); goto fail; }
            memcpy(rows[cur_row][cur_col], in->unescaped_body + f_start, len);
            rows[cur_row][cur_col][len] = '\0';
            cur_row++;
            cur_col = 0;
            f_start = i + 1;
        } else if (f_start < end_offset && in->unescaped_body[i] == MSG_SEP_COL) {
            size_t len = i - f_start;
            rows[cur_row][cur_col] = (char*)malloc(len + 1);
            if (!rows[cur_row][cur_col]) { for (size_t k = 0; k < cur_row; k++) { for (size_t m = 0; rows[k][m]; m++) free(rows[k][m]); free(rows[k]); } for (size_t k = 0; k < hcnt; k++) free(hdr[k]); free(hdr); free(rows); goto fail; }
            memcpy(rows[cur_row][cur_col], in->unescaped_body + f_start, len);
            rows[cur_row][cur_col][len] = '\0';
            cur_col++;
            f_start = i + 1;
        }
    }

    *out_headers = hdr; *out_hcnt = hcnt;
    *out_rows = rows; *out_rcnt = rcnt;
    return 0;

fail:
    if (hdr) { for (size_t k = 0; k < hcnt; k++) free(hdr[k]); free(hdr); }
    if (rows) {
        for (size_t i = 0; i < rcnt; i++) {
            if (rows[i]) { for (size_t j = 0; rows[i][j]; j++) free(rows[i][j]); free(rows[i]); }
        }
        free(rows);
    }
    return MSG_ERR_NO_MEMORY;
}

/* 解析多结果集 body（GS 分隔） */
int msg_parse_body(void *in_ctx) {
    msg_internal_t *in = (msg_internal_t*)in_ctx;
    if (!in->unescaped_body || in->unescaped_len == 0) return 0;

    /* 第一次扫描：找 GS 分隔符，动态数组记录位置 */
    size_t gs_cap = 8;
    size_t *gs_positions = (size_t*)malloc(sizeof(size_t) * gs_cap);
    if (!gs_positions) return MSG_ERR_NO_MEMORY;
    size_t gs_count = 0;

    for (size_t i = 0; i < in->unescaped_len; i++) {
        if (in->unescaped_body[i] == MSG_SEP_RS_GROUP) {
            if (gs_count >= gs_cap) {
                gs_cap *= 2;
                size_t *tmp = (size_t*)realloc(gs_positions, sizeof(size_t) * gs_cap);
                if (!tmp) { free(gs_positions); gs_positions = NULL; return MSG_ERR_NO_MEMORY; }
                gs_positions = tmp;
            }
            gs_positions[gs_count++] = i;
        }
    }

    size_t rs_cnt = gs_count + 1;

    /* 扩容 result_sets 数组 */
    if (ensure_rs_cap(in, rs_cnt) != 0) { free(gs_positions); return MSG_ERR_NO_MEMORY; }
    in->rs_count = rs_cnt;
    in->current_rs = 0;

    /* 计算每个结果集的起止边界 */
    size_t rs_start = 0;
    for (size_t ri = 0; ri < rs_cnt; ri++) {
        size_t rs_end = (ri < gs_count) ? gs_positions[ri] : in->unescaped_len;

        result_set_t *rs = &in->result_sets[ri];
        char **hdr = NULL; size_t hcnt = 0;
        char ***rows = NULL; size_t rcnt = 0;
        int rc = parse_single_rs(in, rs_start, rs_end, &hdr, &hcnt, &rows, &rcnt);
        if (rc != 0) { free(gs_positions); return rc; }
        rs->headers = hdr;
        rs->header_count = hcnt;
        rs->rows = rows;
        rs->row_count = rcnt;

        rs_start = (ri < gs_count) ? gs_positions[ri] + 1 : in->unescaped_len;
    }

    free(gs_positions);
    return 0;
}
