# MsgPacket C++ API 用法指南

## 概述

MsgPacket C++ API 提供 RAII 封装，基于 C API 实现，通过头文件 `msg_packet.hpp` 暴露。

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

### 1. 包含头文件

```cpp
#include "msg_packet.hpp"
```

---

### 2. 构造与销毁

```cpp
// 创建数据包（自动生成 UUID v4 和当前时间戳）
MsgPacket pkt(msg_type);                          // msg_type: MSG_TYPE_REQUEST 等
MsgPacket pkt(msg_type, version);                 // 指定版本

// 从 wire 字节流解码
MsgPacket pkt(wire_data, wire_size);

// 拷贝构造（深拷贝）
MsgPacket pkt2(pkt);
```

`MsgPacket` 实现了 RAII，析构时自动调用 `msg_destroy`。

---

### 3. Header 字段设置

```cpp
void set_func(const std::string& func);           // 设置函数名
void set_msg_id(const std::string& msg_id);       // 手动设置 msg_id
void set_timestamp(const std::string& ts);       // 设置时间戳（空串自动生成）
void set_timestamp();                             // 自动生成当前时间戳
void set_type(uint8_t msg_type);                  // 设置消息类型
void set_format(uint8_t format);                  // 设置格式版本（通常用 'T'）
void set_version(const std::string& version);     // 设置协议版本
```

**示例**:
```cpp
pkt.set_func("subscribe");
pkt.set_timestamp();  // 自动生成当前时间戳
```

---

### 4. Header 字段获取

```cpp
std::string msg_id();          // 返回 32字节UUID
std::string func();            // 返回函数名
std::string timestamp();       // 返回 17字节时间戳
uint8_t msg_type();            // 返回消息类型
uint8_t format();              // 返回格式版本
std::string version();         // 返回协议版本
uint32_t body_len();           // 返回 body 长度
size_t total_len();            // 返回总长度
```

---

### 5. 表头构建

```cpp
// 设置表头（column_count: 列数，headers: 逗号分隔的表头名称）
void set_headers(int column_count, const std::string& headers);

// 追加单个表头字段
void add_header(const std::string& header);

// 获取表头字符串（逗号分隔格式）
std::string get_headers();

// 获取表头字段数量
size_t header_count();
```

**示例**:
```cpp
pkt.set_headers(3, "Symbol,Price,Volume");
std::cout << "Headers: " << pkt.get_headers() << std::endl;
```

---

### 6. 数据行构建

```cpp
// 新增空行
void add_row();

// 格式字符串方式设置当前行各列值（逗号分隔）
// 注意：存在格式字符串安全风险，仅用于可信数据源
template<typename... Args>
void set_row(const char* fmt, Args... args);

// 按 key 设置当前行指定列的值（key 大小写不敏感）
void set_value(const std::string& key, const std::string& value);   // 字符串值
void set_value(const std::string& key, int32_t value);              // int32 值
void set_value(const std::string& key, int64_t value);              // int64 值
void set_value(const std::string& key, double value);              // double 值

// 清除所有已添加的数据行，保留表头
void clear_rows();

// 获取行数
size_t row_count();
```

**示例**:
```cpp
pkt.set_headers(3, "Symbol,Price,Volume");

pkt.add_row();
pkt.set_row("%s,%s,%d", "BTC/USDT", "65000.50", 100);

// 或使用 key-value 方式
pkt.add_row();
pkt.set_value("Symbol", "ETH/USDT");
pkt.set_value("Price", 3500.00);
pkt.set_value("Volume", 500);
```

---

### 7. 提交与获取

```cpp
// 提交打包：序列化 body、转义、计算 CRC32
int finalize();                              // 返回 0 表示成功

// 获取 finalized 后的 wire 数据指针（必须先 finalize）
const void* wire_data();

// 获取 finalized 后的 wire 数据长度（必须先 finalize）
size_t wire_size();

// 获取 finalized 后的 wire 数据（std::vector<uint8_t>）
std::vector<uint8_t> wire_bytes();

// 将 wire 数据转为可读字符串
// 分隔符显示为 <US>/<RS>/<FS>/<ESC>，不可打印字符显示为 '.'
std::string wire_to_string();
```

**示例**:
```cpp
int ret = pkt.finalize();
if (ret != 0) {
    std::cerr << "Finalize failed: " << ret << std::endl;
} else {
    auto wire = pkt.wire_bytes();
    std::cout << "Wire size: " << wire.size() << " bytes" << std::endl;
}
```

---

### 8. 编码/解码

```cpp
// 解码（静态方法）
MsgPacket pkt = MsgPacket::decode(wire_data, wire_size);

// 解码（从 std::vector<uint8_t>）
MsgPacket pkt = MsgPacket::decode(wire_bytes);

// 解码（从 std::string）
MsgPacket pkt = MsgPacket::decode(wire_string);
```

**示例**:
```cpp
auto wire = pkt.wire_bytes();
auto decoded = MsgPacket::decode(wire);
std::cout << "Decoded func: " << decoded.func() << std::endl;
```

---

### 9. 数据遍历

```cpp
// 重置游标到第一行数据之前
void reset_cursor();

// 移动游标到下一行，返回 true 有数据
bool fetch_next();

// 获取当前行号（从 0 开始）
size_t current_row();
```

**示例**:
```cpp
decoded.reset_cursor();
while (decoded.fetch_next()) {
    auto row = decoded.current_row();
    auto symbol = decoded.get_value("Symbol");
    auto price = decoded.get_value("Price");
    std::cout << "Row " << row << ": " << symbol << " @ " << price << std::endl;
}
```

---

### 10. 字段值获取

```cpp
// 按 key 获取当前游标行的值（key 大小写不敏感）
std::string get_value(const std::string& key);

// 按行列索引获取值
std::string get_field(size_t row, size_t col);
```

**示例**:
```cpp
auto symbol = decoded.get_value("Symbol");
auto price = decoded.get_value("Price");
auto first_field = decoded.get_field(0, 1);  // 第一行第二列
```

---

### 11. 多结果集支持（ANSWER 包）

```cpp
// 新增结果集并切换到新结果集
bool add_result_set();

// 切换到下一结果集
bool next_result_set();

// 选择指定结果集（1-based）
int select_result_set(size_t rs_number);

// 获取当前结果集编号（1-based）
size_t result_set();

// 获取结果集数量
size_t result_set_count();
```

**示例**:
```cpp
// 构建多结果集包
pkt.set_headers(2, "Symbol,Price");
pkt.add_row();
pkt.set_value("Symbol", "BTC/USDT");
pkt.set_value("Price", "65000.50");

pkt.add_result_set();
pkt.set_headers(2, "Tag,Note");
pkt.add_row();
pkt.set_value("Tag", "priority");
pkt.set_value("Note", "high-frequency");

// 遍历解码后的多结果集
for (size_t rs = 1; rs <= decoded.result_set_count(); ++rs) {
    if (rs > 1) {
        decoded.next_result_set();
    }
    std::cout << "RS" << rs << ": " << decoded.row_count() << " rows" << std::endl;
}
```

---

## 完整示例：构建并解码数据包

```cpp
#include <iostream>
#include "msg_packet.hpp"

int main() {
    try {
        // 创建请求包
        MsgPacket pkt(MSG_TYPE_REQUEST);
        pkt.set_func("subscribe");

        // 设置表头和数据
        pkt.set_headers(2, "Symbol,Price");
        pkt.add_row();
        pkt.set_value("Symbol", "BTC/USDT");
        pkt.set_value("Price", "65000.50");
        pkt.add_row();
        pkt.set_value("Symbol", "ETH/USDT");
        pkt.set_value("Price", "3500.00");

        // 提交
        int ret = pkt.finalize();
        if (ret != 0) {
            std::cerr << "Finalize failed: " << ret << std::endl;
            return 1;
        }

        // 获取 wire 数据
        auto wire = pkt.wire_bytes();
        std::cout << "Wire size: " << wire.size() << " bytes" << std::endl;

        // 解码验证
        auto decoded = MsgPacket::decode(wire);
        std::cout << "Decoded func: " << decoded.func() << std::endl;
        std::cout << "Headers: " << decoded.header_count()
                  << ", Rows: " << decoded.row_count() << std::endl;

        // 遍历数据
        decoded.reset_cursor();
        while (decoded.fetch_next()) {
            auto row = decoded.current_row();
            auto symbol = decoded.get_value("Symbol");
            auto price = decoded.get_value("Price");
            std::cout << "Row " << row << ": " << symbol << " @ " << price << std::endl;
        }

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
```

---

## 完整示例：构建多结果集包

```cpp
#include <iostream>
#include "msg_packet.hpp"

int main() {
    try {
        MsgPacket pkt(MSG_TYPE_REQUEST);
        pkt.set_func("query");

        // RS1: 账户信息
        pkt.set_headers(2, "Account,Balance");
        pkt.add_row();
        pkt.set_value("Account", "ACC001");
        pkt.set_value("Balance", "100000.00");

        // RS2: 持仓信息
        pkt.add_result_set();
        pkt.set_headers(3, "Symbol,Quantity,Price");
        pkt.add_row();
        pkt.set_value("Symbol", "BTC/USDT");
        pkt.set_value("Quantity", "1.5");
        pkt.set_value("Price", "65000.00");

        // 提交
        pkt.finalize();

        // 解码并遍历
        auto decoded = MsgPacket::decode(pkt.wire_bytes());
        for (size_t rs = 1; rs <= decoded.result_set_count(); ++rs) {
            if (rs > 1) {
                decoded.next_result_set();
            }
            std::cout << "\n=== Result Set " << rs << " ===" << std::endl;
            std::cout << "Headers: " << decoded.get_headers() << std::endl;

            decoded.reset_cursor();
            while (decoded.fetch_next()) {
                auto row = decoded.current_row();
                // 解析表头
                std::string headers = decoded.get_headers();
                std::vector<std::string> header_vec;
                std::stringstream ss(headers);
                std::string header;
                while (std::getline(ss, header, ',')) {
                    header_vec.push_back(header);
                }
                // 获取行数据
                std::cout << "Row " << row << ": ";
                for (const auto& h : header_vec) {
                    std::cout << h << "=" << decoded.get_value(h) << " ";
                }
                std::cout << std::endl;
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

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

---

## 编译说明

C++ demo 需要链接 msgpacket 动态库：

```bash
# Linux
g++ -o demo demo.cpp -L./library/bin/Lnx64 -lmsgpacket -Wl,-rpath,./library/bin/Lnx64

# Windows (MinGW)
g++ -o demo.exe demo.cpp -L./library/bin/x64 -lmsgpacket
```