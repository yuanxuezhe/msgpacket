#ifndef MSG_QUERY_H
#define MSG_QUERY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "msg_internal.h"

/* 解析单个结果集（从 start_offset 开始，到 end_offset 或 GS 之前结束）
 *
 * 两遍扫描：
 *   第一遍：计数 headers 和 rows/cols，避免反复 realloc
 *   第二遍：实际复制数据
 * 注意：第307/316行有 f_start < end_offset 守卫修复空字段泄漏 */
int parse_single_rs(msg_internal_t *in,
                    size_t start_offset, size_t end_offset,
                    char ***out_headers, size_t *out_hcnt,
                    char ****out_rows, size_t *out_rcnt);

/* 解析多结果集 body（GS 分隔） */
int internal_parse_body(msg_internal_t *in);

#endif /* MSG_QUERY_H */
