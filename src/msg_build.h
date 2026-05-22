#ifndef MSG_BUILD_H
#define MSG_BUILD_H

#include <stddef.h>
#include <stdint.h>
#include "msg_internal.h"

/* 编码单个结果集（headers + data）到缓冲区，返回 0 成功
 * 单遍编码：每次写入前检查容量，不够就扩容 */
int encode_rs(uint8_t *buf, size_t *cap, size_t *pos,
              char **headers, size_t header_count,
              char ***rows, size_t row_count, size_t col_count);

/* 确保缓冲区容量足够。msg_finalize 中重复的扩容逻辑统一到此函数。 */
int ensure_body_capacity(uint8_t **buf, size_t *cap, size_t needed);

#endif /* MSG_BUILD_H */
