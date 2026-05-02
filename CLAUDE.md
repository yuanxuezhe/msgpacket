# MsgPacket 协议规范

当前代码的完整规范文档。所有 API 签名、结构体定义、偏移量均与 `src/` 下代码一致。

---

## 1. 物理层：内存布局与对齐规范

由于该结构体涉及跨平台传输（如 64 位服务器与 32 位嵌入式设备），必须严格控制**字节对齐**。

### 结构体定义 (Packed)

字节序转换实现见附录 C。

```c
#include <stdint.h>

#pragma pack(push, 1)

/* 消息头结构（72 字节，packed，无 padding） */
typedef struct {
    char     msg_id[32];       /* 消息唯一标识，32 字节，创建时自动生成 UUID v4 */
    char     ver[8];           /* 协议版本字符串，固定 8 字节，不足补 \0 */
    uint8_t  format;           /* 格式版本（默认 'T'=0x54，可扩展 'J'=JSON） */
    uint8_t  msg_type;         /* 消息类型：MSG_TYPE_REQUEST/ANSWER/PUSH/HEARTBEAT */
    char     timestamp[17];    /* yyyyMMddHHmmssSSS，17 位，无 \0 */
    char     func[8];          /* 函数/操作名，固定 8 字节，不足补 \0 */
    char     msg_code[5];      /* 5 位状态码（如 "00001"），无 \0 */
} msg_header_t;  /* 72 字节（packed） */

/* msg_packet_t — wire 布局：magic[4] + crc32[4] + body_len[4] + header[72] + body[] */
typedef struct {
    char          magic[4];    /* 固定 "YSWY" */
    uint32_t      crc32;       /* CRC32，小端序；计算范围见 5.3 节 */
    uint32_t      body_len;    /* Body 字节数（wire 上为转义后长度，小端序） */
    msg_header_t  header;      /* 72 字节消息头 */
    uint8_t       body[];      /* 柔性数组（C99），包体数据紧跟 header 之后 */
} msg_packet_t;

#pragma pack(pop)
```

### 字段枚举定义

```c
#define MSG_MAGIC            "YSWY"      /* 4 字节固定值，用于协议识别 */
#define MSG_MAGIC_LEN        4
#define MSG_VERSION_DEFAULT  "V1.0"      /* 默认版本 */

/* 格式版本 */
#define MSG_FORMAT_TABLE     'T'         /* 0x54，表格式；可扩展 'J'=JSON */

/* 消息类型 */
#define MSG_TYPE_REQUEST     0x52         /* 'R' 请求 */
#define MSG_TYPE_ANSWER      0x41         /* 'A' 应答 */
#define MSG_TYPE_PUSH        0x50         /* 'P' 推送 */
#define MSG_TYPE_HEARTBEAT   0x48         /* 'H' 心跳 */

/* 状态码（5 位数字字符串） */
#define MSG_CODE_SUCCESS     "00001"      /* 成功 */
#define MSG_CODE_ERROR       "99999"      /* 通用错误 */
#define MSG_CODE_TIMEOUT     "99998"      /* 超时 */

/* 字段数量上限（防止恶意构造耗尽内存） */
#define MSG_MAX_HEADERS      256
#define MSG_MAX_ROWS         65536
#define MSG_MAX_FIELD_LEN    4096
#define MSG_MAX_BODY_LEN     (1024 * 1024)

/* 转义序列 */
#define MSG_ESC_CHAR_US     0x5F         /* '_' -> 0x1F */
#define MSG_ESC_CHAR_RS     0x5E         /* '^' -> 0x1E */
#define MSG_ESC_CHAR_FS     0x5C         /* '\' -> 0x1C */
#define MSG_ESC_CHAR_ESC    0x5B         /* '[' -> 0x1B */

/* 分隔符 */
#define MSG_SEP_COL         0x1F         /* US - 列分隔 */
#define MSG_SEP_ROW         0x1E         /* RS - 行分隔 */
#define MSG_SEP_SECTION     0x1C         /* FS - 区隔表头与数据 */

/* 错误码 */
#define MSG_ERR_NULL_PTR           -1
#define MSG_ERR_INVALID_MAGIC     -2
#define MSG_ERR_CRC_MISMATCH      -3
#define MSG_ERR_BUFFER_TOO_SMALL  -4
#define MSG_ERR_INVALID_FORMAT    -5
#define MSG_ERR_INVALID_MSG_TYPE  -6
#define MSG_ERR_ESCAPE_SEQUENCE   -7
#define MSG_ERR_NO_DATA           -8
#define MSG_ERR_BODY_TOO_LARGE    -9
#define MSG_ERR_TOO_MANY_HEADERS  -10
#define MSG_ERR_TOO_MANY_ROWS     -11
#define MSG_ERR_FIELD_TOO_LONG    -12
#define MSG_ERR_VERSION_MISMATCH  -13
#define MSG_ERR_NO_MEMORY         -14
#define MSG_ERR_NOT_FINALIZED     -15
```

### wire 缓冲区字段偏移量

所有偏移量使用 `offsetof` 自动计算，确保与结构体定义同步：

```c
#define BODY_LEN_POS         offsetof(msg_packet_t, body_len)          /* 8 */
#define BODY_LEN_LENGTH      4
#define HEAD_MSGID_POS       offsetof(msg_packet_t, header.msg_id)     /* 12 */
#define HEAD_MSGID_LENGTH    32
#define HEAD_VER_POS         offsetof(msg_packet_t, header.ver)        /* 44 */
#define HEAD_VER_LENGTH      8
#define HEAD_FORMAT_POS      offsetof(msg_packet_t, header.format)     /* 52 */
#define HEAD_FORMAT_LENGTH   1
#define HEAD_MSGTYPE_POS     offsetof(msg_packet_t, header.msg_type)   /* 53 */
#define HEAD_MSGTYPE_LENGTH  1
#define HEAD_TIMESTAMP_POS   offsetof(msg_packet_t, header.timestamp)  /* 54 */
#define HEAD_TIMESTAMP_LENGTH 17
#define HEAD_FUNC_POS        offsetof(msg_packet_t, header.func)       /* 71 */
#define HEAD_FUNC_LENGTH     8
#define HEAD_CODE_POS        offsetof(msg_packet_t, header.msg_code)   /* 79 */
#define HEAD_CODE_LENGTH     5
#define HEAD_SIZE            sizeof(msg_header_t)                      /* 72 */

/* body 起始偏移：4(magic) + 4(crc32) + 4(body_len) + 72(header) = 84 */
#define BODY_OFFSET          offsetof(msg_packet_t, body)              /* 84 */
```

### 边界界定

* **包格式（wire layout）**：`[magic(4)][crc32(4)][body_len(4)][msg_id(32)][ver(8)][format(1)][msg_type(1)][timestamp(17)][func(8)][msg_code(5)][body(n)]`
* **包体起始地址**：`BODY_OFFSET`（84 字节），即 magic + crc32 + body_len + header 之后。
* **包结束边界**：`BODY_OFFSET + body_len`（直接按 body_len 截断，无需扫描）。
* **心跳包边界**：心跳包（`msg_type=H`）的 `body_len` 必须为 0，Body 为空。
* **Trailing delimiter 规则**：最后一行数据不需要以 [RS] 终止，遇到 body_len 边界即结束。
* **Body 长度限制**：`body_len` 不得超过 `MSG_MAX_BODY_LEN`（1MB）。

---

## 2. 逻辑层：Body 分隔符体系与转义机制

采用 **ASCII 分级管理方案**，与 Base64、纯文本、数值数据不冲突。

| 级别 | 符号名称 | 十六进制 | 宏定义 | 用途 |
| :--- | :--- | :--- | :--- | :--- |
| **Level 1** | **FS** (File Separator) | `0x1C` | `MSG_SEP_SECTION` | 区隔表头与数据 |
| **Level 2** | **RS** (Record Separator) | `0x1E` | `MSG_SEP_ROW` | 行分隔 |
| **Level 3** | **US** (Unit Separator) | `0x1F` | `MSG_SEP_COL` | 列分隔 |

### 转义机制

| 原始字节 | 编码后 | 说明 |
| :--- | :--- | :--- |
| `0x1F` (US) | `0x1B 0x5F` | ESC + `_` |
| `0x1E` (RS) | `0x1B 0x5E` | ESC + `^` |
| `0x1C` (FS) | `0x1B 0x5C` | ESC + `\` |
| `0x1B` (ESC) | `0x1B 0x5B` | ESC + `[` |

**编码规则**：发送时遇到分隔符或 `0x1B`，替换为转义序列。
**解码规则**：解析时遇到 `0x1B` 则将下一字节还原为原始值。
**长度变化**：wire 上的 `body_len` 为转义后长度；`msg_decode()` 还原后将内部 `body_len` 更新为未转义长度。
**错误处理**：孤立 ESC（末尾无后一字节）或无效后缀 → `MSG_ERR_ESCAPE_SEQUENCE`。

---

## 3. 包体内部详细格式 (Schema)

### 结构模型

**单行表头 + 单行数据（最后一行无 [RS]）：**
`[Field1][US][Field2][US][Field3][FS][Val1][US][Val2][US][Val3]`

**单行表头 + 多行数据（最后一行无 [RS]）：**
`[Field1][US][Field2][US][Field3][FS][Val1][US][Val2][US][Val3][RS][Val4][US][Val5][US][Val6]`

**多行表头（表头行之间用 [RS] 分隔）：**
`[Field1][US][Field2][RS][Field3][US][Field4][FS][Val1][US][Val2][RS][Val3][US][Val4]`

---

## 4. 解析算法流程

内部实现位于 `msg_api.c`，采用单次扫描索引法：

1. **头校验**：检查 magic "YSWY"，读取 body_len，验证 CRC32（范围：body_len[4] + header[72] + body[n]）
2. **区域定位**：扫描 body 定位 FS（`0x1C`），分割表头区和数据区
3. **字段索引**：解析出 `header_fields` 和 `data_rows`（`field_desc_t` 数组），不修改原始数据

解析结果结构（内部使用）：
```c
typedef struct {
    size_t offset;
    size_t len;
} field_desc_t;
```

---

## 5. 安全与健壮性建议

### 5.1 安全前提声明

本协议假设运行在可信/加密通道之上（如 TLS/WireGuard）。CRC32 不是加密哈希，仅提供数据完整性校验。如需独立安全保护，应增加 AEAD 加密层、时间戳校验窗口和消息序列号。

### 5.2 字节序规则

**统一小端序（Little-Endian）**。仅 `body_len` 和 `crc32` 两个 `uint32_t` 字段需要 LE 转换。`timestamp` 和 `msg_code` 为 ASCII 字符数组，无需字节序转换。`magic` 为固定字节序列。

### 5.3 CRC 计算规则

* **计算范围**：`body_len(4) + header(72) + body(n)`。不含 `magic(4)` 和 `crc32(4)` 自身。
* **计算时机**：`msg_finalize()` 中先转义 body，再计算 CRC。
* **wire 格式 body_len 语义**：wire 上为转义后长度。`msg_decode()` 还原后更新为未转义长度。

**编码流程**：原始 body → 转义 → 计算 CRC(body_len+header+转义body) → 设置 crc32 → 发送

**解码流程**：接收 → 读取 crc32 → 计算 CRC(body_len+header+转义body) → 比对 → 转义还原 → 更新 body_len → 交付

### 5.4 心跳机制

空闲时发送心跳包（`format='T', body_len=0, msg_type='H'`），建议间隔 5-30 秒。

### 5.5 字段边界校验

| 函数 | 校验规则 | 错误码 |
|------|----------|--------|
| `msg_set_headers` | 表头数 > `MSG_MAX_HEADERS` | `MSG_ERR_TOO_MANY_HEADERS` |
| `msg_set_headers` | 单个表头 > `MSG_MAX_FIELD_LEN` | `MSG_ERR_FIELD_TOO_LONG` |
| `msg_begin_row` | 行数 > `MSG_MAX_ROWS` | `MSG_ERR_TOO_MANY_ROWS` |
| `msg_set_value_str` | 单个字段 > `MSG_MAX_FIELD_LEN` | `MSG_ERR_FIELD_TOO_LONG` |
| `msg_decode` | `body_len` > `MSG_MAX_BODY_LEN` | `MSG_ERR_BODY_TOO_LARGE` |

### 5.6 线程安全性

1. 同一 `msg_packet_t` 实例不可并发调用修改操作
2. 已 finalized 的包可多线程并发读取
3. 并发写入需用 `msg_clone()` 创建独立副本

---

## 6. API 接口设计

所有 API 统一在 `msg_api.h` 中声明，纯 C 风格，支持 Python/Go/Rust 等多语言 FFI 调用。表头 key 大小写不敏感。

### 6.1 创建与销毁

```c
msg_packet_t* msg_create(uint8_t msg_type, const char *version);
void          msg_destroy(msg_packet_t *packet);
msg_packet_t* msg_clone(const msg_packet_t *packet);
```

`msg_create` 自动设置 magic="YSWY"、生成 UUID v4 msg_id、生成当前时间戳、设置 msg_code="00001"。version 传 NULL 默认为 "V1.0"。

### 6.2 Header 字段设置

```c
int msg_set_msg_id(msg_packet_t *packet, const char *msg_id);
int msg_set_func(msg_packet_t *packet, const char *func);
int msg_set_type(msg_packet_t *packet, uint8_t msg_type);

/* 字符串方式设状态码（"00001"~"99999"），NULL 默认 "00001" */
int msg_set_code(msg_packet_t *packet, const char *code);

/* 整数方式设状态码（0~99999），超出范围返回 MSG_ERR_INVALID_FORMAT */
int msg_set_code_int(msg_packet_t *packet, int32_t code);

/* 设置时间戳（17 位字符串），NULL 或空串自动生成当前时间 */
int msg_set_timestamp(msg_packet_t *packet, const char *timestamp);

int msg_set_format(msg_packet_t *packet, uint8_t format);
int msg_set_version(msg_packet_t *packet, const char *version);
```

### 6.3 Header 字段获取

```c
const char* msg_get_msg_id(const msg_packet_t *packet);      /* 32 字节 */
const char* msg_get_func(const msg_packet_t *packet);         /* 8 字节 */
const char* msg_get_version(const msg_packet_t *packet);      /* 8 字节 */
uint8_t     msg_get_type(const msg_packet_t *packet);
const char* msg_get_code(const msg_packet_t *packet);         /* 5 字节 */
const char* msg_get_timestamp(const msg_packet_t *packet);    /* 17 字节 */
uint8_t     msg_get_format(const msg_packet_t *packet);
uint32_t    msg_get_body_len(const msg_packet_t *packet);     /* 已转本地字节序 */
size_t      msg_get_total_len(const msg_packet_t *packet);    /* BODY_OFFSET + body_len */
```

### 6.4 表头构建

```c
/* 批量设置表头，headers 为逗号分隔字符串，column_count 为列数 */
int msg_set_headers(msg_packet_t *packet, int column_count, const char *headers);

/* 追加单个表头字段（多行表头场景） */
int msg_add_header(msg_packet_t *packet, const char *header);

/* 获取表头（逗号分隔格式），out 缓冲区容量由 *out_len 传入 */
int msg_get_headers(const msg_packet_t *packet, char *out, size_t *out_len);
```

### 6.5 数据行构建

```c
int msg_begin_row(msg_packet_t *packet);  /* 新增空行 */

/* 格式字符串方式整行设置（%s 逗号分隔，与表头列数一致） */
int msg_set_row(msg_packet_t *packet, const char *fmt, ...);

/* Key-Value 方式逐列设置（key 大小写不敏感） */
int msg_set_value_str(msg_packet_t *packet, const char *key, const char *value);
int msg_set_value_i32(msg_packet_t *packet, const char *key, int32_t value);
int msg_set_value_i64(msg_packet_t *packet, const char *key, int64_t value);
int msg_set_value_double(msg_packet_t *packet, const char *key, double value);

int msg_clear_rows(msg_packet_t *packet);  /* 清除所有数据行，保留表头 */
```

### 6.6 提交与获取

```c
int          msg_finalize(msg_packet_t *packet);  /* 序列化、转义、CRC，之后只读 */
const void*  msg_data(const msg_packet_t *packet);  /* finalized 后有效 */
size_t       msg_size(const msg_packet_t *packet);
```

### 6.7 编码/解码

```c
int  msg_encode(const msg_packet_t *packet, void **out_buf, size_t *out_len);
int  msg_decode(const void *buf, size_t len, msg_packet_t **out_packet);
void msg_free_buffer(void *buf);  /* 释放 msg_encode / msg_wire_to_string 分配的缓冲区 */

/* wire 数据 → 可读字符串（分隔符→<US>/<RS>/<FS>/<ESC>，不可打印→#）
 * 从 msg_id 开始，跳过 magic(4) + crc32(4) + body_len(4) */
char* msg_wire_to_string(const msg_packet_t *packet);
```

### 6.8 数据遍历

```c
bool   msg_fetch_next(msg_packet_t *packet);  /* 游标移到下一行 */
void   msg_reset_cursor(msg_packet_t *packet);
size_t msg_get_current_row(const msg_packet_t *packet);
```

### 6.9 字段值获取

```c
/* 按 key 获取当前游标行（key 大小写不敏感，out_val 无 \0 终止） */
int msg_get_value_str(msg_packet_t *packet, const char *key, const char **out_val, size_t *out_len);
int msg_get_value_i32(msg_packet_t *packet, const char *key, int32_t *out_val);
int msg_get_value_i64(msg_packet_t *packet, const char *key, int64_t *out_val);
int msg_get_value_double(msg_packet_t *packet, const char *key, double *out_val);

/* 按行列索引获取 */
int msg_get_field(msg_packet_t *packet, size_t row, size_t col,
                  const char **out_val, size_t *out_len);
```

### 6.10 统计

```c
size_t msg_get_header_count(const msg_packet_t *packet);
size_t msg_get_row_count(const msg_packet_t *packet);
```

### 6.11 使用示例

**打包：**

```c
msg_packet_t *req = msg_create(MSG_TYPE_REQUEST, "V1.0");
msg_set_func(req, "getData");
msg_set_code_int(req, 1);              /* 整数方式 */
msg_set_timestamp(req, NULL);          /* 自动生成 */

msg_set_headers(req, 4, "Symbol,Price,Volume,Time");

msg_begin_row(req);
msg_set_value_str(req, "Symbol", "BTC/USDT");
msg_set_value_str(req, "Price", "65000.5");
msg_set_value_double(req, "Volume", 1.2);
msg_set_value_i64(req, "Time", 1717000000000LL);

msg_finalize(req);
/* 发送 msg_data(req), msg_size(req) */
msg_destroy(req);
```

**解包：**

```c
msg_packet_t *received = NULL;
msg_decode(data, len, &received);

printf("func=%.8s code=%.5s\n", msg_get_func(received), msg_get_code(received));

while (msg_fetch_next(received)) {
    const char *sym; size_t sym_len;
    msg_get_value_str(received, "Symbol", &sym, &sym_len);
    printf("%.*s\n", (int)sym_len, sym);
}

msg_destroy(received);
```

---

## 7. 内部实现说明

### 7.1 内存布局

`msg_create()` 实际分配布局：

```
[msg_internal_t* ptr (sizeof(void*) 字节)] [msg_packet_t]
                                             ^── 返回给用户的指针
```

`msg_internal_t` 包含运行时状态（headers、rows、wire_buf、unescaped_body、字段索引、游标等），与线上 `msg_packet_t` 分离，确保 `msg_packet_t` 只包含 wire 格式字段。

### 7.2 内部结构（msg_api.c 内定义）

```c
typedef struct {
    bool     finalized;
    char   **headers;         /* 表头名称数组 */
    size_t   header_count;
    char   **rows;            /* 行数据（逗号分隔字符串） */
    size_t   row_count;
    uint8_t *wire_buf;        /* finalized 后的完整 wire 缓冲区 */
    size_t   wire_size;
    uint8_t *unescaped_body;  /* 转义还原后的 body */
    size_t   unescaped_len;
    field_desc_t *header_fields;
    size_t   header_field_count;
    field_desc_t *data_rows;
    size_t   data_row_count;
    size_t   cursor_row;      /* SIZE_MAX = 未开始 */
} msg_internal_t;
```

### 7.3 辅助工具（msg_util.c/h）

| 函数 | 说明 |
|------|------|
| `void msg_generate_uuid_v4(char out[32])` | 生成 32 字节 UUID v4（无连字符大写十六进制） |
| `void crc32_init(void)` | 初始化 CRC32 查找表 |
| `uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)` | 增量计算 CRC32 |
| `void msg_copy_fixed_field(char *dest, const char *src, size_t max_len)` | 拷贝固定长度字段（memset + memcpy） |
| `uint8_t* msg_escape(const uint8_t *raw, size_t raw_len, size_t *out_len)` | 转义编码 |
| `uint8_t* msg_unescape(const uint8_t *data, size_t len, size_t *out_len)` | 转义解码 |

---

## 附录 A：UUID v4 生成算法

```c
void msg_generate_uuid_v4(char out[32]) {
    uint8_t bytes[16];
    for (int i = 0; i < 16; i++)
        bytes[i] = (uint8_t)(secure_rand() % 256);
    bytes[6] = (bytes[6] & 0x0F) | 0x40;  /* 版本 4 */
    bytes[8] = (bytes[8] & 0x3F) | 0x80;  /* 变体 10xx */
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 16; i++) {
        out[i * 2]     = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0F];
    }
}
```

---

## 附录 B：CRC32 实现参考

```c
#define CRC32_POLY  0xEDB88320UL
#define CRC32_INIT  0xFFFFFFFFUL

static uint32_t crc32_table[256];

void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (CRC32_POLY & ~(crc & 1));
        crc32_table[i] = crc;
    }
}

uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    crc ^= CRC32_INIT;
    for (size_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ CRC32_INIT;
}
```

---

## 附录 C：字节序转换

```c
/* 仅 body_len 和 crc32 需要 LE 转换。timestamp 和 msg_code 为 ASCII 字符数组，无需转换 */
#if defined(__GNUC__) || defined(__clang__)
    #define MSG_HTOLE32(x) (x)
    #define MSG_LE32TOH(x) (x)
#else
    /* 通用实现见 src/msg_byteorder.h */
#endif
```

---

## 项目结构

```
msgpacket/
├── CLAUDE.md             项目知识库（本文件）
├── CMakeLists.txt        CMake 编译配置
│
├── src/                  C 语言 API 源码
│   ├── msg_byteorder.h   字节序转换
│   ├── msg_packet.h      协议结构体定义（packed）
│   ├── msg_util.c/h      UUID/CRC32/转义工具
│   └── msg_api.c/h       统一 API（创建/销毁/设置/获取/编码/解码）
│
├── library/              编译产物（运行时库）
│   ├── include/           公开头文件
│   ├── bin/x64/           libmsgpacket.dll
│   └── lib/x64/           libmsgpacket.a
│
├── demo/
│   ├── c/                 C demo（demo_builder / demo_parser / demo_full_cycle）
│   ├── cpp/               C++ RAII demo（msg_packet.hpp + demo.cpp）
│   ├── python/            Python ctypes demo（msgpacket.py + demo.py）
│   └── rust/              Rust FFI demo（libloading, main.rs）
│
└── bin/x64/               可执行文件输出
```

### 编译说明

```bash
# Windows (MinGW)
mkdir build && cd build
cmake -G "MinGW Makefiles" ..
mingw32-make -j4

# 运行 C demo
./bin/x64/demo_builder.exe
./bin/x64/demo_parser.exe
./bin/x64/demo_full_cycle.exe

# 运行 C++ demo
./bin/x64/demo_cpp.exe

# 运行 Python demo
python demo/python/demo.py

# 运行 Rust demo
cd demo/rust && cargo run
```

### 已知问题/限制

1. **msg_set_row 使用逗号分隔**：格式字符串内部使用逗号作为列分隔符，`msg_finalize()` 中将逗号替换为 US (0x1F) 写入 wire。
2. **行解析兼容逗号和 US**：`row_get_field_at()` 同时支持 US 和逗号作为列分隔符。
3. **msg_wire_to_string 跳过前 12 字节**：跳过 magic(4) + crc32(4) + body_len(4)，从 msg_id 开始。
4. **timestamp 无 \0 终止**：timestamp 为固定 17 字节 `yyyMMddHHmmssSSS`，读取时必须限制长度为 17。
5. **msg_code 无 \0 终止**：msg_code 为固定 5 字节，读取时必须限制长度为 5。
6. **body_len LE 转换**：`body_len` 和 `crc32` 是仅有的需要小端序转换的字段。
