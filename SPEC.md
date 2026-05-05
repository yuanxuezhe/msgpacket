# MsgPacket 协议规范

**版本**：V1.0
**语言**：纯 C，符合 C99 标准
**用途**：跨平台二进制消息协议，支持表格式数据打包/解包、多结果集、心跳检测

---

## 1. 概述

MsgPacket 是一种基于二进制转义的表格式消息协议，采用固定头部 + 表格式 Body 的结构，支持多结果集应答、字段转义、CRC32 完整性校验。协议设计为紧凑字节流，适用于嵌入式设备、服务器间通信等场景。

---

## 2. 物理层：内存布局

### 2.1 结构体定义（#pragma pack(push, 1)）

```c
#include <stdint.h>

#pragma pack(push, 1)

typedef struct {
    char     msg_id[33];       /* 消息唯一标识，32字节UUID + \0 */
    char     ver[9];          /* 协议版本，8字节 + \0 */
    uint8_t  format;           /* 格式版本：'T'(0x54) 表格式，'J'=JSON（扩展） */
    uint8_t  msg_type;         /* 消息类型：MSG_TYPE_REQUEST/ANSWER/PUSH/HEARTBEAT */
    char     timestamp[18];    /* yyyyMMddHHmmssSSS，17字节 + \0 */
    char     func[9];          /* 函数/操作名，8字节 + \0 */
    char     msg_code[6];      /* 5位状态码 + \0（如 "00001\0"） */
} msg_header_t;  /* 78 字节 */

typedef struct {
    char          magic[4];    /* 固定 "YSWY" */
    uint32_t      crc32;       /* CRC32，小端序 */
    uint32_t      body_len;    /* Body 字节数（wire 上为转义后长度），小端序 */
    msg_header_t  header;      /* 78 字节 */
    uint8_t       body[];      /* 柔性数组（C99），包体紧跟 header 之后 */
} msg_packet_t;

#pragma pack(pop)
```

### 2.2 字段偏移量（offsetof 计算）

| 字段 | 偏移量 | 长度 | 说明 |
|------|--------|------|------|
| `magic` | 0 | 4 | 固定 "YSWY" |
| `crc32` | 4 | 4 | 小端序 |
| `body_len` | 8 | 4 | 小端序 |
| `header.msg_id` | 12 | 33 | |
| `header.ver` | 45 | 9 | |
| `header.format` | 54 | 1 | |
| `header.msg_type` | 55 | 1 | |
| `header.timestamp` | 56 | 18 | |
| `header.func` | 74 | 9 | |
| `header.msg_code` | 83 | 6 | |
| `body` | 101 | — | 柔性数组 |

**包总长**：`BODY_OFFSET(101) + body_len`

### 2.3 字节序

**仅 `body_len` 和 `crc32`（两个 uint32_t）需要小端序转换**。`timestamp` 和 `msg_code` 为 ASCII 字符数组，无需字节序转换。

### 2.4 CRC32 计算规则

- **计算范围**：`body_len(4) + header(78) + body(n)`，不含 magic(4) 和 crc32(4) 自身
- **计算时机**：`msg_finalize()` 中先转义 body，再计算 CRC
- **多项式**：`0xEDB88320`（IEEE 802.3 标准），初始值 `0xFFFFFFFF`，最终异或 `0xFFFFFFFF`

---

## 3. 逻辑层：分隔符与转义机制

### 3.1 ASCII 分级分隔符体系

| 级别 | 符号 | 十六进制 | 宏定义 | 用途 |
|------|------|---------|--------|------|
| Level 0 | GS | `0x1D` | `MSG_SEP_RS_GROUP` | 多结果集分隔 |
| Level 1 | FS | `0x1C` | `MSG_SEP_SECTION` | 区隔表头与数据 |
| Level 2 | RS | `0x1E` | `MSG_SEP_ROW` | 行分隔 |
| Level 3 | US | `0x1F` | `MSG_SEP_COL` | 列分隔 |

### 3.2 转义规则

**编码**（发送时）：遇到分隔符或 `0x1B` → 替换为转义序列

| 原始字节 | 编码后 |
|----------|--------|
| `0x1F` (US) | `0x1B 0x5F` |
| `0x1E` (RS) | `0x1B 0x5E` |
| `0x1C` (FS) | `0x1B 0x5C` |
| `0x1B` (ESC) | `0x1B 0x5B` |
| `0x1D` (GS) | `0x1B 0x5D` |

**解码**（接收时）：遇到 `0x1B` → 将下一字节还原为原始值
**错误处理**：孤立 ESC 或无效后缀 → `MSG_ERR_ESCAPE_SEQUENCE(-7)`

**注意**：`wire` 上的 `body_len` 为**转义后**长度；`msg_decode()` 还原后将内部 `body_len` 更新为**未转义**长度。

---

## 4. 包体格式（Body Schema）

### 4.1 单结果集

```
[Field1][US][Field2][US][Field3][FS][Val1][US][Val2][US][Val3]
[Field1][US][Field2][US][Field3][FS][Val1][US][Val2][US][Val3][RS][Val4][US][Val5][US][Val6]
```

- `[FS]` 区隔表头区与数据区
- `[RS]` 分隔多行数据
- 最后一行数据**不**以 `[RS]` 终止，遇到 `body_len` 边界即结束

### 4.2 多结果集（ANSWER 包）

```
[RS1_Header][FS][RS1_Data1][RS][RS1_Data2][GS][RS2_Header][FS][RS2_Data1][RS][RS2_Data2]
```

- `<GS>` (0x1D) 分隔两个结果集
- 每个结果集有独立的表头区和数据区
- 结果集编号：1-based

---

## 5. 错误码

| 错误码 | 宏名 | 说明 |
|--------|------|------|
| -1 | `MSG_ERR_NULL_PTR` | 空指针参数 |
| -2 | `MSG_ERR_INVALID_MAGIC` | magic 不匹配 |
| -3 | `MSG_ERR_CRC_MISMATCH` | CRC32 校验失败 |
| -4 | `MSG_ERR_BUFFER_TOO_SMALL` | 输出缓冲区不足 |
| -5 | `MSG_ERR_INVALID_FORMAT` | 格式版本不支持 |
| -6 | `MSG_ERR_INVALID_MSG_TYPE` | 消息类型无效 |
| -7 | `MSG_ERR_ESCAPE_SEQUENCE` | 转义序列错误 |
| -8 | `MSG_ERR_NO_DATA` | 无数据（未解码或未构建） |
| -9 | `MSG_ERR_BODY_TOO_LARGE` | body 超过 1MB |
| -10 | `MSG_ERR_TOO_MANY_HEADERS` | 表头字段超过 256 |
| -11 | `MSG_ERR_TOO_MANY_ROWS` | 行数超过 65536 |
| -12 | `MSG_ERR_FIELD_TOO_LONG` | 单字段超过 4096 字节 |
| -13 | `MSG_ERR_VERSION_MISMATCH` | 协议版本不匹配 |
| -14 | `MSG_ERR_NO_MEMORY` | 内存分配失败 |
| -15 | `MSG_ERR_NOT_FINALIZED` | 包未完成序列化 |

---

## 6. API 接口

### 6.1 常量定义

```c
#define MSG_MAGIC            "YSWY"
#define MSG_VERSION_DEFAULT  "V1.0"
#define MSG_FORMAT_TABLE      'T'          /* 0x54 */
#define MSG_TYPE_REQUEST      0x52         /* 'R' */
#define MSG_TYPE_ANSWER       0x41         /* 'A' */
#define MSG_TYPE_PUSH         0x50         /* 'P' */
#define MSG_TYPE_HEARTBEAT    0x48         /* 'H' */
#define MSG_CODE_SUCCESS      "00001"
#define MSG_CODE_ERROR        "99999"
#define MSG_CODE_TIMEOUT      "99998"

#define MSG_MAX_HEADERS      256
#define MSG_MAX_ROWS         65536
#define MSG_MAX_FIELD_LEN    4096
#define MSG_MAX_BODY_LEN     (1024 * 1024)

#define HEAD_TIMESTAMP_LENGTH 17
#define HEAD_FUNC_LENGTH      8
#define HEAD_CODE_LENGTH      5
```

### 6.2 创建与销毁

```c
msg_packet_t* msg_create(uint8_t msg_type, const char *version);
void          msg_destroy(msg_packet_t *packet);
msg_packet_t* msg_clone(const msg_packet_t *packet);
```

`msg_create` 自动设置 magic="YSWY"、生成 UUID v4 msg_id、生成当前时间戳、设置 msg_code="00001"。version 传 NULL 默认为 "V1.0"。

### 6.3 Header 字段设置

```c
int msg_set_msg_id(msg_packet_t *packet, const char *msg_id);
int msg_set_func(msg_packet_t *packet, const char *func);
int msg_set_type(msg_packet_t *packet, uint8_t msg_type);
int msg_set_code(msg_packet_t *packet, const char *code);
int msg_set_code_int(msg_packet_t *packet, int32_t code);
int msg_set_timestamp(msg_packet_t *packet, const char *timestamp);  /* NULL/空串自动生成 */
int msg_set_format(msg_packet_t *packet, uint8_t format);
int msg_set_version(msg_packet_t *packet, const char *version);
```

### 6.4 Header 字段获取

```c
const char* msg_get_msg_id(const msg_packet_t *packet);      /* 33 字节（含 \0） */
const char* msg_get_func(const msg_packet_t *packet);        /* 9 字节（含 \0） */
const char* msg_get_version(const msg_packet_t *packet);     /* 9 字节（含 \0） */
const char* msg_get_code(const msg_packet_t *packet);         /* 6 字节（含 \0） */
const char* msg_get_timestamp(const msg_packet_t *packet);    /* 18 字节（含 \0） */
uint8_t     msg_get_type(const msg_packet_t *packet);
uint8_t     msg_get_format(const msg_packet_t *packet);
uint32_t    msg_get_body_len(const msg_packet_t *packet);     /* 已转本地字节序 */
size_t      msg_get_total_len(const msg_packet_t *packet);    /* BODY_OFFSET + body_len */
```

### 6.5 表头构建

```c
int msg_set_headers(msg_packet_t *packet, int column_count, const char *headers);
int msg_add_header(msg_packet_t *packet, const char *header);
int msg_get_headers(const msg_packet_t *packet, char *out, size_t *out_len);
```

### 6.6 数据行构建

```c
int msg_add_row(msg_packet_t *packet);
int msg_set_row(msg_packet_t *packet, const char *fmt, ...);
int msg_set_value_str(msg_packet_t *packet, const char *key, const char *value);
int msg_set_value_i32(msg_packet_t *packet, const char *key, int32_t value);
int msg_set_value_i64(msg_packet_t *packet, const char *key, int64_t value);
int msg_set_value_double(msg_packet_t *packet, const char *key, double value);
int msg_clear_rows(msg_packet_t *packet);
```

### 6.7 提交与获取

```c
int          msg_finalize(msg_packet_t *packet);
const void*  msg_data(const msg_packet_t *packet);
size_t       msg_size(const msg_packet_t *packet);
```

### 6.8 编码/解码

```c
int  msg_encode(const msg_packet_t *packet, void **out_buf, size_t *out_len);
int  msg_decode(const void *buf, size_t len, msg_packet_t **out_packet);
void msg_free_buffer(void *buf);
char* msg_wire_to_string(const msg_packet_t *packet);
```

### 6.9 数据遍历

```c
bool   msg_fetch_next(msg_packet_t *packet);
void   msg_reset_cursor(msg_packet_t *packet);
size_t msg_get_current_row(const msg_packet_t *packet);
```

### 6.10 字段值获取

```c
int msg_get_value_str(msg_packet_t *packet, const char *key, const char **out_val, size_t *out_len);
int msg_get_value_i32(msg_packet_t *packet, const char *key, int32_t *out_val);
int msg_get_value_i64(msg_packet_t *packet, const char *key, int64_t *out_val);
int msg_get_value_double(msg_packet_t *packet, const char *key, double *out_val);

int msg_get_field(msg_packet_t *packet, size_t row, size_t col,
                  const char **out_val, size_t *out_len);
```

### 6.11 统计与结果集

```c
size_t msg_get_header_count(const msg_packet_t *packet);
size_t msg_get_row_count(const msg_packet_t *packet);

size_t msg_get_result_set(const msg_packet_t *packet);
bool   msg_add_result_set(msg_packet_t *packet);
bool   msg_next_result_set(msg_packet_t *packet);
int    msg_select_result_set(msg_packet_t *packet, size_t rs_number);
size_t msg_get_result_set_count(const msg_packet_t *packet);
```

---

## 7. 线程安全规则

1. 同一 `msg_packet_t` 实例**不可**并发调用修改操作
2. 已 `finalized` 的包可**多线程并发读取**
3. 并发写入需用 `msg_clone()` 创建独立副本

---

## 8. 关键实现说明

### 8.1 字符串字段读取

所有 Header 字符串字段（`msg_id`、`ver`、`timestamp`、`func`、`msg_code`）均为 **`\0` 终止**，可直接使用 `strlen`/`strcmp` 等标准字符串函数，无需按长度截断。

```c
// 正确用法
const char *ts = msg_get_timestamp(packet);  // 直接用 strlen/strcmp，\0 自动终止
if (strcmp(ts, "20250505000000000") == 0) { ... }

// 错误用法
memcmp(msg_get_timestamp(packet), "20250505000000000", 18); // 不需要按长度
```

### 8.2 `func` / `msg_code` 字段

`func`（9 字节）和 `msg_code`（6 字节）均为 **`\0` 终止**，可直接用 `strlen`/`strcmp`。

### 8.3 内存布局

`msg_create()` 分配的布局：

```
[msg_internal_t* ptr (sizeof(void*) 字节)] [msg_packet_t]
                                              ^── 返回给用户的指针
```

`msg_internal_t` 包含运行时状态（headers、rows、wire_buf、unescaped_body、字段索引、游标等），与 wire `msg_packet_t` 分离。

### 8.4 内部状态转换

| 阶段 | 状态 | 说明 |
|------|------|------|
| `msg_create` | 构建阶段 | headers/rows 在内部 `msg_internal_t` 中，不在 wire 上 |
| `msg_finalize` | finalized | body 被转义、CRC 被计算，wire_buf 填充完毕 |
| `msg_decode` | 解析阶段 | wire → unescaped → 字段索引，wire_buf 存储原始 wire 数据 |

---

## 9. 项目结构

```
msgpacket/
├── CLAUDE.md              项目知识库
├── SPEC.md                本协议规范文档
├── CMakeLists.txt         CMake 编译配置
│
├── src/
│   ├── msg_byteorder.h    字节序转换
│   ├── msg_packet.h       协议结构体定义（packed）
│   ├── msg_util.c/h       UUID v4 / CRC32 / 转义工具
│   └── msg_api.c/h        统一 API
│
├── demo/
│   ├── c/                 C demo
│   └── cpp/               C++ RAII demo
│
└── tests/
    ├── test_msgpacket.c   cmocka 单元测试
    └── CMakeLists.txt     测试构建配置
```

---

## 10. 编译说明

```bash
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON -DBUILD_DEMOS=ON
make -j$(nproc)
ctest --output-on-failure
```

**依赖**：`libcmocka-dev`（apt-get install）

**编译选项**：`-fPIC -fno-common -Wpedantic -fvisibility=hidden`
