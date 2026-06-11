# MsgPacket Python API 用法指南

## 概述

MsgPacket Python API 通过 ctypes 调用 C 动态库，实现跨语言的消息打包/解包。本指南基于 `msgpacket/__init__.py` 模块。

## 消息类型常量

| 常量 | 值 | 说明 |
|------|-----|------|
| `MSG_TYPE_REQUEST` | `0x52` ('R') | 请求包 |
| `MSG_TYPE_ANSWER` | `0x41` ('A') | 应答包 |
| `MSG_TYPE_PUSH` | `0x50` ('P') | 推送包 |
| `MSG_TYPE_HEARTBEAT` | `0x48` ('H') | 心跳包 |

## 错误码

| 错误码 | 说明 |
|--------|------|
| -1 | 空指针参数 |
| -2 | magic 不匹配 |
| -3 | CRC32 校验失败 |
| -4 | 输出缓冲区不足 |
| -5 | 格式版本不支持 |
| -6 | 消息类型无效 |
| -7 | 转义序列错误 |
| -8 | 无数据 |
| -9 | body 超过 1MB |
| -10 | 表头字段超过 256 |
| -11 | 行数超过 65536 |
| -12 | 单字段超过 4096 字节 |
| -13 | 协议版本不匹配 |
| -14 | 内存分配失败 |
| -15 | 包未完成序列化 |

---

## 核心 API

### 1. 导入

```python
from msgpacket import (
    MsgPacket,
    MSG_TYPE_REQUEST, MSG_TYPE_ANSWER, MSG_TYPE_PUSH, MSG_TYPE_HEARTBEAT,
    msg_type_name
)
```

---

### 2. 构造与销毁

```python
# 创建数据包（自动生成 UUID v4 和当前时间戳）
pkt = MsgPacket(msg_type)  # msg_type: MSG_TYPE_REQUEST 等

# 从 wire 字节流解码
pkt = MsgPacket.decode(wire_data: bytes)
```

**示例**:
```python
pkt = MsgPacket(MSG_TYPE_REQUEST)
```

---

### 3. Header 字段设置

```python
pkt.set_func(func: str)           # 设置函数名
pkt.set_msg_id(msg_id: str)       # 手动设置 msg_id
pkt.set_timestamp(ts: str)        # 设置时间戳（None/空串自动生成）
pkt.set_type(msg_type: int)       # 设置消息类型
pkt.set_format(fmt: int)          # 设置格式版本（通常用 ord('T')）
pkt.set_version(ver: str)         # 设置协议版本（默认 "V1.0"）
```

**示例**:
```python
pkt.set_func("subscribe")
pkt.set_timestamp(None)  # 自动生成当前时间戳
```

---

### 4. Header 字段获取

```python
pkt.msg_id()          # 返回 str，32字节UUID
pkt.func()            # 返回 str，函数名
pkt.timestamp()       # 返回 str，17字节时间戳
pkt.msg_type()        # 返回 int，消息类型
pkt.format()          # 返回 int，格式版本
pkt.version()         # 返回 str，协议版本
pkt.body_len()        # 返回 int，body长度
pkt.total_len()       # 返回 int，总长度
```

---

### 5. 表头构建

```python
# 设置表头（column_count: 列数，headers: 逗号分隔的表头名称）
pkt.set_headers(column_count: int, headers: str)

# 获取表头字符串（逗号分隔格式）
hdr = pkt.get_headers()  # -> str

# 获取表头字段数量
count = pkt.header_count()  # -> int
```

**示例**:
```python
pkt.set_headers(3, "Symbol,Price,Volume")
print(pkt.get_headers())  # "Symbol,Price,Volume"
```

---

### 6. 数据行构建

```python
# 新增空行
pkt.add_row()

# 设置当前行数据（逗号分隔的格式字符串）
# 注意：存在格式字符串安全风险，仅用于可信数据源
pkt.set_row(fmt: str, *args)

# 按 key 设置当前行指定列的值（key 大小写不敏感）
pkt.set_value(key: str, value: str)      # 字符串值
pkt.set_value(key: str, value: int)      # int32 值
pkt.set_value(key: str, value: float)    # double 值

# 清除所有已添加的数据行，保留表头
pkt.clear_rows()

# 获取行数
count = pkt.row_count()  # -> int
```

**示例**:
```python
pkt.set_headers(3, "Symbol,Price,Volume")

pkt.add_row()
pkt.set_row("%s,%s,%d", "BTC/USDT", "65000.50", 100)

# 或使用 key-value 方式
pkt.add_row()
pkt.set_value("Symbol", "ETH/USDT")
pkt.set_value("Price", 3500.00)
pkt.set_value("Volume", 500)
```

---

### 7. 提交与获取

```python
# 提交打包：序列化 body、转义、计算 CRC32
ret = pkt.finalize()  # 返回 0 表示成功

# 获取 finalized 后的 wire 数据
wire_data = pkt.wire_data_bytes()  # -> bytes

# 获取 wire 数据长度
wire_size = pkt.wire_size()  # -> int

# 获取 wire 数据长度（字节）
wire_size = len(pkt.wire_data_bytes())
```

**示例**:
```python
ret = pkt.finalize()
if ret != 0:
    print(f"Finalize failed: {ret}")
else:
    wire = pkt.wire_data_bytes()
```

---

### 8. 编码/解码

```python
# 解码（类方法）
pkt = MsgPacket.decode(wire_data: bytes)

# 将 wire 数据转为可读字符串
# 分隔符显示为 <US>/<RS>/<FS>/<ESC>，不可打印字符显示为 '.'
wire_str = pkt.wire_to_string()  # -> str
```

**示例**:
```python
# 解码
pkt = MsgPacket.decode(wire_data)
print(f"func: {pkt.func()}, type: {chr(pkt.msg_type())}")
```

---

### 9. 数据遍历

```python
# 重置游标到第一行数据之前
pkt.reset_cursor()

# 移动游标到下一行，返回 True 有数据
has_next = pkt.fetch_next()  # -> bool

# 获取当前行号（从 0 开始）
row = pkt.current_row()  # -> int
```

**示例**:
```python
pkt.reset_cursor()
while pkt.fetch_next():
    row = pkt.current_row()
    # 处理当前行...
```

---

### 10. 字段值获取

```python
# 按 key 获取当前游标行的值（key 大小写不敏感）
value = pkt.get_value(key: str)  # -> str

# 按行列索引获取值
value = pkt.get_field(row: int, col: int)  # -> str

# 获取表头字段数量
count = pkt.header_count()  # -> int
```

**示例**:
```python
pkt.reset_cursor()
while pkt.fetch_next():
    symbol = pkt.get_value("Symbol")
    price = pkt.get_value("Price")
    print(f"Row {pkt.current_row()}: {symbol} @ {price}")

# 按索引获取
val = pkt.get_field(0, 1)  # 第一行第二列
```

---

### 11. 多结果集支持（ANSWER 包）

```python
# 新增结果集并切换到新结果集
ok = pkt.add_result_set()  # -> bool

# 切换到下一结果集
ok = pkt.next_result_set()  # -> bool

# 选择指定结果集（1-based）
ret = pkt.select_result_set(rs_number: int)  # -> 0 成功

# 获取当前结果集编号（1-based）
rs = pkt.result_set()  # -> int

# 获取结果集数量
count = pkt.result_set_count()  # -> int
```

**示例**:
```python
# 构建多结果集包
pkt.set_headers(2, "Symbol,Price")
pkt.add_row()
pkt.set_value("Symbol", "BTC/USDT")
pkt.set_value("Price", "65000.50")

# 添加第二个结果集
pkt.add_result_set()
pkt.set_headers(2, "Tag,Note")
pkt.add_row()
pkt.set_value("Tag", "priority")
pkt.set_value("Note", "high-frequency")

# 遍历解码后的多结果集
for rs in range(1, pkt.result_set_count() + 1):
    if rs > 1:
        pkt.next_result_set()
    print(f"RS{rs}: {pkt.row_count()} rows")
```

---

## 完整示例：构建并解码数据包

```python
from msgpacket import MsgPacket, MSG_TYPE_REQUEST, MSG_TYPE_ANSWER

# 创建请求包
pkt = MsgPacket(MSG_TYPE_REQUEST)
pkt.set_func("subscribe")
pkt.set_headers(2, "Symbol,Price")
pkt.add_row()
pkt.set_value("Symbol", "BTC/USDT")
pkt.set_value("Price", "65000.50")
pkt.add_row()
pkt.set_value("Symbol", "ETH/USDT")
pkt.set_value("Price", "3500.00")

# 提交
ret = pkt.finalize()
if ret != 0:
    print(f"Finalize failed: {ret}")
else:
    wire_data = pkt.wire_data_bytes()
    print(f"Wire size: {len(wire_data)} bytes")

    # 解码验证
    decoded = MsgPacket.decode(wire_data)
    print(f"Decoded func: {decoded.func()}")
    print(f"Headers: {decoded.header_count()}, Rows: {decoded.row_count()}")

    # 遍历数据
    decoded.reset_cursor()
    while decoded.fetch_next():
        row = decoded.current_row()
        symbol = decoded.get_value("Symbol")
        price = decoded.get_value("Price")
        print(f"Row {row}: {symbol} @ {price}")
```

---

## 完整示例：构建多结果集包

```python
from msgpacket import MsgPacket, MSG_TYPE_REQUEST

# 创建多结果集包
pkt = MsgPacket(MSG_TYPE_REQUEST)
pkt.set_func("query")

# RS1: 账户信息
pkt.set_headers(2, "Account,Balance")
pkt.add_row()
pkt.set_value("Account", "ACC001")
pkt.set_value("Balance", "100000.00")

# RS2: 持仓信息
pkt.add_result_set()
pkt.set_headers(3, "Symbol,Quantity,Price")
pkt.add_row()
pkt.set_value("Symbol", "BTC/USDT")
pkt.set_value("Quantity", "1.5")
pkt.set_value("Price", "65000.00")

# 提交
pkt.finalize()

# 解码并遍历
decoded = MsgPacket.decode(pkt.wire_data_bytes())
for rs in range(1, decoded.result_set_count() + 1):
    if rs > 1:
        decoded.next_result_set()
    print(f"\n=== Result Set {rs} ===")
    print(f"Headers: {decoded.get_headers()}")
    decoded.reset_cursor()
    while decoded.fetch_next():
        row_data = [decoded.get_value(h) for h in decoded.get_headers().split(",")]
        print(f"Row {decoded.current_row()}: {row_data}")
```

---

## 安装

```bash
pip install .
```

或开发模式：

```bash
pip install -e .
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

1. 同一 `MsgPacket` 实例**不可**并发调用修改操作
2. 已 `finalize` 的包可**多线程并发读取**
3. 并发写入需用 `MsgPacket` 创建独立副本

---

## 转义规则

| 原始字节 | 编码后 |
|----------|--------|
| `0x1F` (US) | `0x1B 0x5F` |
| `0x1E` (RS) | `0x1B 0x5E` |
| `0x1C` (FS) | `0x1B 0x5C` |
| `0x1B` (ESC) | `0x1B 0x5B` |
| `0x1D` (GS) | `0x1B 0x5D` |