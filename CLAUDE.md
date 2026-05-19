# MsgPacket 项目知识库

项目协议规范详见 `SPEC.md`。

---

## 项目结构

```
msgpacket/
├── SPEC.md               协议规范文档
├── CLAUDE.md             本文件，项目知识库
├── CMakeLists.txt        CMake 编译配置（项目版本 1.1.0）
│
├── src/                  C 语言 API 源码
│   ├── msg_byteorder.h  字节序转换（仅 body_len/crc32 需要转换）
│   ├── msg_packet.h     协议结构体定义（#pragma pack(push,1)）
│   ├── msg_util.c/h     UUID v4 / CRC32 / 转义/反转义工具
│   └── msg_api.c/h      统一 API（创建/销毁/构建/解析/finalize）
│
├── library/              编译产物（运行时库）
│   ├── include/         公开头文件（msg_api.h / msg_util.h / msg_byteorder.h）
│   ├── bin/x64/         动态库 (.dll)
│   ├── bin/Lnx64/       动态库 (.so)
│   └── lib/x64/         静态库 (.a)
│
├── tests/
│   ├── minunit.h        零依赖跨平台测试框架
│   └── test_msgpacket.c MinUnit 单元测试
│
└── demo/                 多语言 FFI 示例
    ├── c/                C demo（demo_builder / demo_parser / demo_full_cycle）
    ├── cpp/              C++ RAII demo + msg_packet.hpp
    ├── python/            Python ctypes demo（builder/rpc/publisher）
    └── rust/              Rust FFI demo + RPC 示例（msgpacket.rs）
```

---

## 编译

```bash
# Linux/macOS
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON -DBUILD_DEMOS=ON
make -j4
./bin/Lnx64/test_msgpacket

# Windows (MSYS2/MinGW)
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DBUILD_TESTS=ON -DBUILD_DEMOS=ON
mingw32-make -j4
./bin/x64/test_msgpacket.exe
```

**依赖**：仅 C 标准库（测试框架 MinUnit 为头文件，无外部依赖）

---

## API 概览

| 类别 | 函数 |
|------|------|
| 创建/销毁 | `msg_create` / `msg_destroy` / `msg_clone` |
| Header | `msg_set_msg_id` / `msg_set_func` / `msg_set_timestamp` / `msg_set_version` |
| 表头 | `msg_set_headers` / `msg_add_header` |
| 数据行 | `msg_add_row` / `msg_set_row` / `msg_set_value_str/i32/i64/double` |
| 提交 | `msg_finalize` / `msg_data` / `msg_size` |
| 编码/解码 | `msg_encode` / `msg_decode` / `msg_free_buffer` |
| 遍历 | `msg_fetch_next` / `msg_reset_cursor` / `msg_get_current_row` |
| 字段获取 | `msg_get_value_str/i32/i64/double` / `msg_get_field` |
| 多结果集 | `msg_add_result_set` / `msg_next_result_set` / `msg_select_result_set` |

---

## 已知限制

1. **msg_set_row 使用逗号分隔**：内部用逗号作为列分隔符，`msg_finalize()` 将逗号替换为 US (0x1F) 写入 wire
2. **行解析兼容逗号和 US**：同时支持两种分隔符
3. **timestamp 无 \\0 终止**：固定 17 字节 `yyyyMMddHHmmssSSS`
4. **body_len 和 crc32 使用小端序**：其他字段为 ASCII/字节数组，无需字节序转换
5. **msg_set_row 格式字符串风险**：仅用于可信数据源

---

## 线程安全规则

1. 同一 `msg_packet_t` 实例**不可**并发调用修改操作
2. 已 `finalized` 的包可**多线程并发读取**
3. 并发写入需用 `msg_clone()` 创建独立副本

---

## CRC32 计算规则

- **多项式**：`0xEDB88320`（IEEE 802.3 标准）
- **初始值**：`0xFFFFFFFF`，最终异或 `0xFFFFFFFF`
- **计算范围**：`body_len(4) + header(72) + body(n)`，不含 magic(4) 和 crc32(4) 自身

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

## Wire 布局

```
偏移 0   : magic[4]      = "YSWY"
偏移 4   : crc32[4]      = 小端序
偏移 8   : body_len[4]   = 小端序
偏移 12  : header        = msg_header_t (72字节)
偏移 83  : body[]        = 柔性数组
总长     : BODY_OFFSET(83) + body_len
```

Header 内字符串字段（msg_id/ver/timestamp/func）均为 `\\0` 终止，可直接使用 `strlen`/`strcmp`。

---

## 项目约定

- 所有字符串字段（msg_id/ver/func/timestamp）末尾保证有 `\\0`
- `timestamp` 为 17 字节 ASCII 字符 `yyyyMMddHHmmssSSS` + `\\0`（共 18 字节）
- 多结果集分隔符为 GS (0x1D)，ANSWER 包专用
- `wire` 上 `body_len` 为**转义后**长度