#ifndef MSG_RESULTSET_H
#define MSG_RESULTSET_H

#include <stddef.h>

/* 单个结果集（构建阶段和解码后均使用相同结构） */
typedef struct {
    char   **headers;         /* 表头名称数组 */
    size_t   header_count;
    char   ***rows;           /* 行数据：rows[row][col] = 字段值字符串指针 */
    size_t   row_count;
} result_set_t;

#endif /* MSG_RESULTSET_H */