#ifndef MSG_UTIL_H
#define MSG_UTIL_H

#include <stdint.h>
#include <stddef.h>

/* UUID v4 生成（随机生成）
 * 格式：xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
 * 输出：32 字节无连字符大写十六进制 */
void msg_generate_uuid_v4(char out[32]);

/* 初始化 CRC32 表（仅需调用一次） */
void crc32_init(void);

/* 计算 CRC32 */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len);

/* 转义编码：返回转义后数据（调用者需释放） */
uint8_t* msg_escape(const uint8_t *data, size_t len, size_t *out_len);

/* 转义解码：返回解码后数据（调用者需释放） */
uint8_t* msg_unescape(const uint8_t *data, size_t len, size_t *out_len);

/* 复制固定长度字段并补零 */
void msg_copy_fixed_field(char *dest, const char *src, size_t max_len);

#endif /* MSG_UTIL_H */
