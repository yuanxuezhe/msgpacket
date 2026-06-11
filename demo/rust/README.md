# MsgPacket Rust API 用法指南

## 概述

MsgPacket Rust API 通过 `libloading` 调用 C 动态库，提供类型安全的 RAII 封装。本指南基于 `src/msgpacket.rs` 模块。

## 消息类型常量

| 常量 | 值 | 说明 |
|------|-----|------|
| `MSG_TYPE_REQUEST` | `0x52` ('R') | 请求包 |
| `MSG_TYPE_ANSWER` | `0x41` ('A') | 应答包 |
| `MSG_TYPE_PUSH` | `0x50` ('P') | 推送包 |
| `MSG_TYPE_HEARTBEAT` | `0x48` ('H') | 心跳包 |

## 错误类型

```rust
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum MsgError {
    NullPtr          = -1,
    InvalidMagic     = -2,
    CrcMismatch      = -3,
    BufferTooSmall   = -4,
    InvalidFormat    = -5,
    InvalidMsgType   = -6,
    EscapeSequence   = -7,
    NoData           = -8,
    BodyTooLarge     = -9,
    TooManyHeaders   = -10,
    TooManyRows      = -11,
    FieldTooLong     = -12,
    VersionMismatch  = -13,
    NoMemory         = -14,
    NotFinalized     = -15,
    Unknown          = 0,
}
```

---

## 核心 API

### 1. 初始化

```rust
use msgpacket::{load_library, Packet, MSG_TYPE_REQUEST};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    // 加载动态库（自动搜索多种路径）
    load_library()?;

    // 你的代码...
    Ok(())
}
```

**动态库搜索路径**:
- Windows: `library/bin/x64/libmsgpacket.dll`, `libmsgpacket.dll` 等
- Linux: `library/bin/Lnx64/libmsgpacket.so`, `libmsgpacket.so` 等
- macOS: `library/bin/MacOS64/libmsgpacket.dylib` 等

---

### 2. 构造与销毁

```rust
// 创建新的数据包
pub fn new(msg_type: u8, version: &str) -> Result<Self, MsgError>

// 从 wire 字节流解码
pub fn decode(data: &[u8]) -> Result<Self, MsgError>
```

**示例**:
```rust
let pkt = Packet::new(MSG_TYPE_REQUEST, "V1.0")?;
// 自动生成 UUID v4 和当前时间戳
```

`Packet` 实现了 `Drop`，超出作用域时自动调用 `msg_destroy`。

---

### 3. Header 字段设置

```rust
pub fn set_func(&self, func: &str) -> Result<(), MsgError>
pub fn set_msg_id(&self, msg_id: &str) -> Result<(), MsgError>
pub fn set_timestamp(&self, ts: Option<&str>) -> Result<(), MsgError>  // None 表示自动生成
pub fn set_type(&self, t: u8) -> Result<(), MsgError>
pub fn set_format(&self, fmt: u8) -> Result<(), MsgError>
```

**示例**:
```rust
pkt.set_func("subscribe")?;
pkt.set_timestamp(None)?;  // 自动生成当前时间戳
```

---

### 4. Header 字段获取

```rust
pub fn msg_id(&self) -> String          // 32字节UUID
pub fn func(&self) -> String            // 函数名（trim后的）
pub fn timestamp(&self) -> String       // 17字节时间戳
pub fn msg_type(&self) -> u8            // 消息类型
pub fn format(&self) -> u8              // 格式版本
pub fn body_len(&self) -> u32           // body长度
pub fn total_len(&self) -> usize        // 总长度
```

---

### 5. 表头构建

```rust
// 设置表头（column_count: 列数，headers: 逗号分隔的表头名称）
pub fn set_headers(&self, ncols: i32, headers: &str) -> Result<(), MsgError>

// 获取表头字段数量
pub fn header_count(&self) -> usize

// 获取表头字符串（逗号分隔格式）
pub fn get_headers(&self) -> String
```

**示例**:
```rust
pkt.set_headers(3, "Symbol,Price,Volume")?;
println!("Headers: {}", pkt.get_headers());
```

---

### 6. 数据行构建

```rust
// 新增空行
pub fn add_row(&self) -> Result<(), MsgError>

// 按 key 设置当前行指定列的值（key 大小写不敏感）
pub fn set_value(&self, key: &str, value: &str) -> Result<(), MsgError>

// 获取行数
pub fn row_count(&self) -> usize
```

**注意**: Rust API 目前仅提供 `set_value`（字符串），如需其他类型请先转换。

**示例**:
```rust
pkt.set_headers(3, "Symbol,Price,Volume")?;

pkt.add_row()?;
pkt.set_value("Symbol", "BTC/USDT")?;
pkt.set_value("Price", "65000.50")?;
pkt.set_value("Volume", "100")?;

pkt.add_row()?;
pkt.set_value("Symbol", "ETH/USDT")?;
pkt.set_value("Price", "3500.00")?;
pkt.set_value("Volume", "500")?;
```

---

### 7. 提交与获取

```rust
// 提交打包：序列化 body、转义、计算 CRC32
pub fn finalize(&self) -> Result<(), MsgError>

// 获取 wire 数据（finalize 后有效）
pub fn wire_data(&self) -> &[u8]

// 获取 wire 数据字节长度
pub fn wire_size(&self) -> usize

// 将 wire 数据转为可读字符串
// 分隔符显示为 <US>/<RS>/<FS>/<ESC>，不可打印字符显示为 '.'
pub fn wire_to_string(&self) -> String
```

**示例**:
```rust
pkt.finalize()?;
let wire = pkt.wire_data();
println!("Wire size: {} bytes", wire.len());
println!("Wire string: {}", pkt.wire_to_string());
```

---

### 8. 数据遍历

```rust
// 移动游标到下一行，返回 true 有数据
pub fn fetch_next(&self) -> bool

// 重置游标到第一行数据之前
pub fn reset_cursor(&self)

// 获取当前行号（从 0 开始）
pub fn current_row(&self) -> usize
```

**示例**:
```rust
pkt.reset_cursor();
while pkt.fetch_next() {
    let row = pkt.current_row();
    let symbol = pkt.get_value("Symbol");
    let price = pkt.get_value("Price");
    println!("Row {}: {} @ {}", row, symbol, price);
}
```

---

### 9. 字段值获取

```rust
// 按 key 获取当前游标行的值（key 大小写不敏感）
pub fn get_value(&self, key: &str) -> String

// 按行列索引获取值
pub fn get_field(&self, row: usize, col: usize) -> String
```

**示例**:
```rust
let symbol = pkt.get_value("Symbol");
let first_price = pkt.get_field(0, 1);  // 第一行第二列
```

---

### 10. 多结果集支持（ANSWER 包）

```rust
// 新增结果集并切换
pub fn add_result_set(&self) -> bool

// 切换到下一结果集
pub fn next_result_set(&self) -> bool

// 选择指定结果集（1-based）
pub fn select_result_set(&self, rs: usize) -> Result<(), MsgError>

// 获取当前结果集编号（1-based）
pub fn result_set(&self) -> usize

// 获取结果集数量
pub fn result_set_count(&self) -> usize
```

**示例**:
```rust
// 构建多结果集包
pkt.set_headers(2, "Symbol,Price")?;
pkt.add_row()?;
pkt.set_value("Symbol", "BTC/USDT")?;
pkt.set_value("Price", "65000.50")?;

pkt.add_result_set();
pkt.set_headers(2, "Tag,Note")?;
pkt.add_row()?;
pkt.set_value("Tag", "priority")?;
pkt.set_value("Note", "high-frequency")?;

// 遍历解码后的多结果集
for rs in 1..=decoded.result_set_count() {
    if rs > 1 {
        decoded.next_result_set();
    }
    println!("RS{}: {} rows", rs, decoded.row_count());
}
```

---

## 完整示例：构建并解码数据包

```rust
use msgpacket::{load_library, Packet, MSG_TYPE_REQUEST};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    // 初始化库
    load_library()?;

    // 创建请求包
    let pkt = Packet::new(MSG_TYPE_REQUEST, "V1.0")?;
    pkt.set_func("subscribe")?;

    // 设置表头和数据
    pkt.set_headers(2, "Symbol,Price")?;
    pkt.add_row()?;
    pkt.set_value("Symbol", "BTC/USDT")?;
    pkt.set_value("Price", "65000.50")?;
    pkt.add_row()?;
    pkt.set_value("Symbol", "ETH/USDT")?;
    pkt.set_value("Price", "3500.00")?;

    // 提交
    pkt.finalize()?;

    // 获取 wire 数据
    let wire = pkt.wire_data();
    println!("Wire size: {} bytes", wire.len());

    // 解码验证
    let decoded = Packet::decode(wire)?;
    println!("Decoded func: {}", decoded.func());
    println!("Headers: {}, Rows: {}", decoded.header_count(), decoded.row_count());

    // 遍历数据
    decoded.reset_cursor();
    while decoded.fetch_next() {
        let symbol = decoded.get_value("Symbol");
        let price = decoded.get_value("Price");
        println!("Row {}: {} @ {}", decoded.current_row(), symbol, price);
    }

    Ok(())
}
```

---

## 完整示例：构建多结果集包

```rust
use msgpacket::{load_library, Packet, MSG_TYPE_REQUEST};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    load_library()?;

    let pkt = Packet::new(MSG_TYPE_REQUEST, "V1.0")?;
    pkt.set_func("query")?;

    // RS1: 账户信息
    pkt.set_headers(2, "Account,Balance")?;
    pkt.add_row()?;
    pkt.set_value("Account", "ACC001")?;
    pkt.set_value("Balance", "100000.00")?;

    // RS2: 持仓信息
    pkt.add_result_set();
    pkt.set_headers(3, "Symbol,Quantity,Price")?;
    pkt.add_row()?;
    pkt.set_value("Symbol", "BTC/USDT")?;
    pkt.set_value("Quantity", "1.5")?;
    pkt.set_value("Price", "65000.00")?;

    // 提交
    pkt.finalize()?;

    // 解码并遍历
    let decoded = Packet::decode(pkt.wire_data())?;
    for rs in 1..=decoded.result_set_count() {
        if rs > 1 {
            decoded.next_result_set();
        }
        println!("\n=== Result Set {} ===", rs);
        println!("Headers: {}", decoded.get_headers());
        decoded.reset_cursor();
        while decoded.fetch_next() {
            let headers: Vec<&str> = decoded.get_headers().split(',').collect();
            let row_data: Vec<String> = headers.iter()
                .map(|h| decoded.get_value(h))
                .collect();
            println!("Row {}: {:?}", decoded.current_row(), row_data);
        }
    }

    Ok(())
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

1. 同一 `Packet` 实例**不可**并发调用修改操作
2. 已 `finalize` 的包可**多线程并发读取**
3. 并发写入需用 `Packet::new` 创建独立副本

---

## 转义规则

| 原始字节 | 编码后 |
|----------|--------|
| `0x1F` (US) | `0x1B 0x5F` |
| `0x1E` (RS) | `0x1B 0x5E` |
| `0x1C` (FS) | `0x1B 0x5C` |
| `0x1B` (ESC) | `0x1B 0x5B` |
| `0x1D` (GS) | `0x1B 0x5D` |

---

## Cargo.toml 依赖

```toml
[dependencies]
libloading = "0.8"
```

确保动态库在库搜索路径中，或使用相对路径加载。