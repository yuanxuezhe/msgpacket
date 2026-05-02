#ifndef MSG_API_H
#define MSG_API_H

#include <stdbool.h>
#include "msg_packet.h"

/* ============================================= */
/* 创建与销毁 */
/* ============================================= */

/* 创建新数据包，自动生成 UUID v4 作为 msg_id */
msg_packet_t* msg_create(uint8_t msg_type, const char *version);

/* 销毁数据包，释放所有内存 */
void msg_destroy(msg_packet_t *packet);

/* 深拷贝数据包（含内部构建/解析状态） */
msg_packet_t* msg_clone(const msg_packet_t *packet);

/* ============================================= */
/* Header 字段设置 */
/* ============================================= */

int msg_set_msg_id(msg_packet_t *packet, const char *msg_id);
int msg_set_func(msg_packet_t *packet, const char *func);
int msg_set_type(msg_packet_t *packet, uint8_t msg_type);
int msg_set_code(msg_packet_t *packet, const char *code);
int msg_set_code_int(msg_packet_t *packet, int32_t code);
int msg_set_timestamp(msg_packet_t *packet, const char *timestamp);
int msg_set_format(msg_packet_t *packet, uint8_t format);
int msg_set_version(msg_packet_t *packet, const char *version);

/* ============================================= */
/* Header 字段获取 */
/* ============================================= */

const char* msg_get_msg_id(const msg_packet_t *packet);
const char* msg_get_func(const msg_packet_t *packet);
const char* msg_get_version(const msg_packet_t *packet);
uint8_t  msg_get_type(const msg_packet_t *packet);
const char* msg_get_code(const msg_packet_t *packet);
const char* msg_get_timestamp(const msg_packet_t *packet);
uint8_t  msg_get_format(const msg_packet_t *packet);
uint32_t msg_get_body_len(const msg_packet_t *packet);
size_t   msg_get_total_len(const msg_packet_t *packet);

/* ============================================= */
/* 表头构建 */
/* ============================================= */

/* 设置表头（column_count: 列数，headers: 逗号分隔的表头名称）
 * 示例：msg_set_headers(packet, 3, "Symbol,Price,Volume") */
int msg_set_headers(msg_packet_t *packet, int column_count, const char *headers);

/* 追加单个表头字段（用于多行表头场景） */
int msg_add_header(msg_packet_t *packet, const char *header);

/* 获取表头字符串（逗号分隔格式），out 缓冲区容量由 *out_len 传入 */
int msg_get_headers(const msg_packet_t *packet, char *out, size_t *out_len);

/* ============================================= */
/* 数据行构建 */
/* ============================================= */

/* 新增空行，后续调用 msg_set_value_* 或 msg_set_row 填充 */
int msg_begin_row(msg_packet_t *packet);

/* 格式字符串方式设置当前行各列值（逗号分隔，与表头列数一致）
 * 注意：存在格式字符串安全风险，建议仅用于可信数据源 */
int msg_set_row(msg_packet_t *packet, const char *fmt, ...);

/* 按 key 设置当前行指定列的值（key 大小写不敏感） */
int msg_set_value_str(msg_packet_t *packet, const char *key, const char *value);
int msg_set_value_i32(msg_packet_t *packet, const char *key, int32_t value);
int msg_set_value_i64(msg_packet_t *packet, const char *key, int64_t value);
int msg_set_value_double(msg_packet_t *packet, const char *key, double value);

/* 清除所有已添加的数据行，保留表头 */
int msg_clear_rows(msg_packet_t *packet);

/* ============================================= */
/* 提交 */
/* ============================================= */

/* 提交打包：序列化 body、转义、计算 CRC32，之后 packet 只读 */
int msg_finalize(msg_packet_t *packet);

/* 获取 finalized 后的 wire 数据指针 */
const void* msg_data(const msg_packet_t *packet);

/* 获取 finalized 后的 wire 数据长度 */
size_t msg_size(const msg_packet_t *packet);

/* ============================================= */
/* 编码/解码 */
/* ============================================= */

/* 编码为独立缓冲区（调用者需 msg_free_buffer 释放） */
int msg_encode(const msg_packet_t *packet, void **out_buf, size_t *out_len);

/* 从 wire 字节流解码，自动验证 magic/CRC、转义还原、解析 body */
int msg_decode(const void *buf, size_t len, msg_packet_t **out_packet);

/* 释放 msg_encode / msg_wire_to_string 分配的缓冲区 */
void msg_free_buffer(void *buf);

/* 将 packet 的 wire 数据转为可读字符串（分隔符→<US>/<RS>/<FS>/<ESC>，不可打印→'.'）
 * 从 msg_id 开始，跳过 magic + crc32，packet 必须已 finalized，调用者需 msg_free_buffer 释放 */
char* msg_wire_to_string(const msg_packet_t *packet);

/* ============================================= */
/* 数据遍历 */
/* ============================================= */

/* 移动游标到下一行，返回 true 有数据 */
bool msg_fetch_next(msg_packet_t *packet);

/* 重置游标到第一行数据之前 */
void msg_reset_cursor(msg_packet_t *packet);

/* 获取当前行号（从 0 开始） */
size_t msg_get_current_row(const msg_packet_t *packet);

/* ============================================= */
/* 字段值获取（按 key，当前游标行，key 大小写不敏感） */
/* ============================================= */

/* 获取字符串值（out_val 指向内部数据，out_len 为长度，无 \0 终止） */
int msg_get_value_str(msg_packet_t *packet, const char *key, const char **out_val, size_t *out_len);

int msg_get_value_i32(msg_packet_t *packet, const char *key, int32_t *out_val);
int msg_get_value_i64(msg_packet_t *packet, const char *key, int64_t *out_val);
int msg_get_value_double(msg_packet_t *packet, const char *key, double *out_val);

/* ============================================= */
/* 字段值获取（按行列索引） */
/* ============================================= */

int msg_get_field(msg_packet_t *packet, size_t row, size_t col,
                  const char **out_val, size_t *out_len);

/* ============================================= */
/* 统计 */
/* ============================================= */

size_t msg_get_header_count(const msg_packet_t *packet);
size_t msg_get_row_count(const msg_packet_t *packet);

#endif /* MSG_API_H */
