# MsgPacket C API 用法指南

## 概述

MsgPacket 是一种基于二进制转义的表格式消息协议，采用固定头部 + 表格式 Body 的结构，支持多结果集应答、字段转义、CRC32 完整性校验。

## 消息类型常量

| 常量 | 值 | 说明 |
|------|-----|------|
| `MSG_TYPE_REQUEST` | `'R'` (0x52) | 请求包 |
| `MSG_TYPE_ANSWER` | `'A'` (0x41) | 应答包 |
| `MSG_TYPE_PUSH` | `'P'` (0x50) | 推送包 |
| `MSG_TYPE_HEARTBEAT` | `'H'` (0x48) | 心跳包 |

## 错误码

| 错误码 | 宏名 | 说明 |
|--------|------|------|
| -1 | `MSG_ERR_NULL_PTR` | 空指针参数 |
| -2 | `MSG_ERR_INVALID_MAGIC` | magic 不匹配 |
| -3 | `MSG_ERR_CRC_MISMATCH` | CRC32 校验失败 |
| -4 | `MSG_ERR_BUFFER_TOO_SMALL` | 输出缓冲区不足 |
| -5 | `MSG_ERR_INVALID_FORMAT` | 格式版本不支持 |
| -6 | `MSG_ERR_INVALID_MSG_TYPE` | 消息类型无效 |
| -7 | `MSG_ERR_ESCAPE_SEQUENCE` | 转义序列错误 |
| -8 | `MSG_ERR_NO_DATA` | 无数据 |
| -9 | `MSG_ERR_BODY_TOO_LARGE` | body 超过 1MB |
| -10 | `MSG_ERR_TOO_MANY_HEADERS` | 表头字段超过 256 |
| -11 | `MSG_ERR_TOO_MANY_ROWS` | 行数超过 65536 |
| -12 | `MSG_ERR_FIELD_TOO_LONG` | 单字段超过 4096 字节 |
| -13 | `MSG_ERR_VERSION_MISMATCH` | 协议版本不匹配 |
| -14 | `MSG_ERR_NO_MEMORY` | 内存分配失败 |
| -15 | `MSG_ERR_NOT_FINALIZED` | 包未完成序列化 |

---

## 核心 API

### 1. 创建与销毁

```c
#include "msg_api.h"

// 创建数据包（自动生成 UUID v4 和当前时间戳）
msg_packet_t* msg_create(uint8_t msg_type, const char *version);
// version 传 NULL 默认为 "V1.0"

// 销毁数据包
void msg_destroy(msg_packet_t *packet);

// 深拷贝数据包
msg_packet_t* msg_clone(const msg_packet_t *packet);
```

**示例**:
```c
msg_packet_t *pkt = msg_create(MSG_TYPE_REQUEST, "V1.0");
if (!pkt) {
    // 处理错误
}
msg_destroy(pkt);
```

---

### 2. Header 字段设置

```c
int msg_set_msg_id(msg_packet_t *packet, const char *msg_id);      // 手动设置 msg_id
int msg_set_func(msg_packet_t *packet, const char *func);           // 设置函数名
int msg_set_type(msg_packet_t *packet, uint8_t msg_type);            // 设置消息类型
int msg_set_timestamp(msg_packet_t *packet, const char *timestamp); // 设置时间戳（NULL/空串自动生成）
int msg_set_format(msg_packet_t *packet, uint8_t format);           // 设置格式版本（通常用 'T'）
int msg_set_version(msg_packet_t *packet, const char *version);      // 设置协议版本
```

**示例**:
```c
msg_set_func(pkt, "subscribe");
msg_set_timestamp(pkt, NULL);  // 自动生成当前时间戳
```

---

### 3. Header 字段获取

```c
const char* msg_get_msg_id(const msg_packet_t *packet);      // 返回 33 字节（含 \0）
const char* msg_get_func(const msg_packet_t *packet);        // 返回 9 字节（含 \0）
const char* msg_get_version(const msg_packet_t *packet);     // 返回 9 字节（含 \0）
const char* msg_get_timestamp(const msg_packet_t *packet);   // 返回 18 字节（含 \0）
uint8_t     msg_get_type(const msg_packet_t *packet);
uint8_t     msg_get_format(const msg_packet_t *packet);
uint32_t    msg_get_body_len(const msg_packet_t *packet);     // 已转本地字节序
size_t      msg_get_total_len(const msg_packet_t *packet);    // BODY_OFFSET(83) + body_len
```

**注意**: 所有字符串字段均为 `\0` 终止，可直接用 `strlen`/`strcmp`。

---

### 4. 表头构建

```c
// 设置表头（column_count: 列数，headers: 逗号分隔的表头名称）
int msg_set_headers(msg_packet_t *packet, int column_count, const char *headers);

// 追加单个表头字段
int msg_add_header(msg_packet_t *packet, const char *header);

// 获取表头字符串（逗号分隔格式）
int msg_get_headers(const msg_packet_t *packet, char *out, size_t *out_len);
```

**示例**:
```c
msg_set_headers(pkt, 3, "Symbol,Price,Volume");
```

---

### 5. 数据行构建

```c
// 新增空行
int msg_add_row(msg_packet_t *packet);

// 格式字符串方式设置当前行各列值（逗号分隔）
// 注意：存在格式字符串安全风险，仅用于可信数据源
int msg_set_row(msg_packet_t *packet, const char *fmt, ...);

// 按 key 设置当前行指定列的值（key 大小写不敏感）
int msg_set_value_str(msg_packet_t *packet, const char *key, const char *value);
int msg_set_value_i32(msg_packet_t *packet, const char *key, int32_t value);
int msg_set_value_i64(msg_packet_t *packet, const char *key, int64_t value);
int msg_set_value_double(msg_packet_t *packet, const char *key, double value);

// 清除所有已添加的数据行，保留表头
int msg_clear_rows(msg_packet_t *packet);
```

**示例**:
```c
msg_add_row(pkt);
msg_set_row(pkt, "%s,%s,%d", "BTC/USDT", "65000.50", 100);

// 或使用 key-value 方式
msg_add_row(pkt);
msg_set_value_str(pkt, "Symbol", "ETH/USDT");
msg_set_value_double(pkt, "Price", 3500.00);
msg_set_value_i64(pkt, "Volume", 500);
```

---

### 6. 提交与获取

```c
// 提交打包：序列化 body、转义、计算 CRC32，之后 packet 只读
int msg_finalize(msg_packet_t *packet);

// 获取 finalized 后的 wire 数据指针（必须先 msg_finalize）
const void* msg_data(const msg_packet_t *packet);

// 获取 finalized 后的 wire 数据长度（必须先 msg_finalize）
size_t msg_size(const msg_packet_t *packet);
```

**示例**:
```c
msg_finalize(pkt);
const void *wire_data = msg_data(pkt);
size_t wire_size = msg_size(pkt);
```

---

### 7. 编码/解码

```c
// 编码为独立缓冲区（调用者需 msg_free_buffer 释放）
int msg_encode(const msg_packet_t *packet, void **out_buf, size_t *out_len);

// 从 wire 字节流解码，自动验证 magic/CRC、转义还原、解析 body
int msg_decode(const void *buf, size_t len, msg_packet_t **out_packet);

// 释放 msg_encode / msg_wire_to_string 分配的缓冲区
void msg_free_buffer(void *buf);

// 将 packet 的 wire 数据转为可读字符串
// 分隔符显示为 <US>/<RS>/<FS>/<ESC>，不可打印字符显示为 '.'
// 调用者需 msg_free_buffer 释放
char* msg_wire_to_string(const msg_packet_t *packet);
```

**示例**:
```c
// 解码
msg_packet_t *decoded = NULL;
int ret = msg_decode(wire_data, wire_size, &decoded);
if (ret == 0) {
    // 处理解码后的包
    msg_destroy(decoded);
}

// 编码
void *buf = NULL;
size_t buf_len = 0;
msg_encode(pkt, &buf, &buf_len);
// 使用 buf...
msg_free_buffer(buf);
```

---

### 8. 数据遍历

```c
// 移动游标到下一行，返回 true 有数据
bool msg_fetch_next(msg_packet_t *packet);

// 重置游标到第一行数据之前
void msg_reset_cursor(msg_packet_t *packet);

// 获取当前行号（从 0 开始）
size_t msg_get_current_row(const msg_packet_t *packet);
```

**示例**:
```c
msg_reset_cursor(pkt);
while (msg_fetch_next(pkt)) {
    size_t row = msg_get_current_row(pkt);
    // 处理当前行...
}
```

---

### 9. 字段值获取

```c
// 按 key 获取当前游标行的值（key 大小写不敏感）
int msg_get_value_str(msg_packet_t *packet, const char *key, const char **out_val, size_t *out_len);
int msg_get_value_i32(msg_packet_t *packet, const char *key, int32_t *out_val);
int msg_get_value_i64(msg_packet_t *packet, const char *key, int64_t *out_val);
int msg_get_value_double(msg_packet_t *packet, const char *key, double *out_val);

// 按行列索引获取值
int msg_get_field(msg_packet_t *packet, size_t row, size_t col,
                  const char **out_val, size_t *out_len);
```

**示例**:
```c
const char *val = NULL;
size_t val_len = 0;
msg_get_value_str(pkt, "Symbol", &val, &val_len);
printf("Symbol: %.*s\n", (int)val_len, val);

// 按索引获取
const char *field_val = NULL;
size_t field_len = 0;
msg_get_field(pkt, 0, 1, &field_val, &field_len);
```

---

### 10. 统计

```c
size_t msg_get_header_count(const msg_packet_t *packet);
size_t msg_get_row_count(const msg_packet_t *packet);
```

---

### 11. 多结果集支持（ANSWER 包）

```c
// 获取当前结果集编号（1-based，即 RS1=1, RS2=2, ...）
size_t msg_get_result_set(const msg_packet_t *packet);

// 新增结果集并切换到新结果集，返回 false 表示已达上限
bool msg_add_result_set(msg_packet_t *packet);

// 切换到下一结果集（1-based），返回 false 表示没有更多结果集
bool msg_next_result_set(msg_packet_t *packet);

// 选择指定结果集（传入 1-based 编号），超出范围返回错误码
int msg_select_result_set(msg_packet_t *packet, size_t rs_number);

// 获取结果集数量
size_t msg_get_result_set_count(const msg_packet_t *packet);
```

**示例**:
```c
// 构建多结果集包
msg_set_headers(pkt, 2, "Symbol,Price");
msg_add_row(pkt);
msg_set_row(pkt, "%s,%s", "BTC/USDT", "65000.50");

// 添加第二个结果集
msg_add_result_set(pkt);
msg_set_headers(pkt, 2, "Tag,Note");
msg_add_row(pkt);
msg_set_row(pkt, "%s,%s", "priority", "high-frequency");

// 遍历解码后的多结果集
for (size_t rs = 1; rs <= msg_get_result_set_count(decoded); rs++) {
    if (rs > 1) {
        msg_next_result_set(decoded);
    }
    // 处理当前结果集...
}
```

---

## 完整示例：构建并解码数据包

```c
#include <stdio.h>
#include "msg_api.h"

int main(void) {
    // 创建请求包
    msg_packet_t *pkt = msg_create(MSG_TYPE_REQUEST, "V1.0");
    if (!pkt) {
        fprintf(stderr, "Failed to create packet\n");
        return 1;
    }

    // 设置 Header
    msg_set_func(pkt, "subscribe");
    msg_set_timestamp(pkt, NULL);  // 自动生成时间戳

    // 设置表头和数据
    msg_set_headers(pkt, 2, "Symbol,Price");
    msg_add_row(pkt);
    msg_set_row(pkt, "%s,%s", "BTC/USDT", "65000.50");
    msg_add_row(pkt);
    msg_set_row(pkt, "%s,%s", "ETH/USDT", "3500.00");

    // 提交
    int ret = msg_finalize(pkt);
    if (ret != 0) {
        fprintf(stderr, "Finalize failed: %d\n", ret);
        msg_destroy(pkt);
        return 1;
    }

    // 获取 wire 数据
    const void *wire_data = msg_data(pkt);
    size_t wire_size = msg_size(pkt);
    printf("Wire size: %zu bytes\n", wire_size);

    // 解码验证
    msg_packet_t *decoded = NULL;
    ret = msg_decode(wire_data, wire_size, &decoded);
    if (ret == 0) {
        printf("Decoded func: %.8s\n", msg_get_func(decoded));
        printf("Headers: %zu, Rows: %zu\n",
               msg_get_header_count(decoded), msg_get_row_count(decoded));
        msg_destroy(decoded);
    }

    msg_destroy(pkt);
    return 0;
}
```

---

## Wire 格式布局

```
偏移 0   : magic[4]      = "YSWY"
偏移 4   : crc32[4]      = 小端序
偏移 8   : body_len[4]   = 小端序
偏移 12  : header        = msg_header_t (72字节)
偏移 83  : body[]        = 柔性数组
总长     : BODY_OFFSET(83) + body_len
```

**仅 `body_len` 和 `crc32` 需要小端序转换**。

---

## 线程安全规则

1. 同一 `msg_packet_t` 实例**不可**并发调用修改操作
2. 已 `finalized` 的包可**多线程并发读取**
3. 并发写入需用 `msg_clone()` 创建独立副本

---

## 转义规则

| 原始字节 | 编码后 |
|----------|--------|
| `0x1F` (US) | `0x1B 0x5F` |
| `0x1E` (RS) | `0x1B 0x5E` |
| `0x1C` (FS) | `0x1B 0x5C` |
| `0x1B` (ESC) | `0x1B 0x5B` |
| `0x1D` (GS) | `0x1B 0x5D` |