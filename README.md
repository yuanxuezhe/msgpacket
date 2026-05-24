# MsgPacket

**MsgPacket** 是一种高效的二进制表格式消息协议，纯 C 实现（符合 C99），零外部依赖。支持多结果集、字段转义、CRC32 校验，适用于嵌入式设备、服务器通信等场景。

[![CI](https://github.com/msgpacket/msgpacket/actions/workflows/ci.yml/badge.svg)](https://github.com/msgpacket/msgpacket/actions/workflows/ci.yml)
[![ASAN](https://github.com/msgpacket/msgpacket/actions/workflows/ci.yml/badge.svg?job=asan)](https://github.com/msgpacket/msgpacket/actions/workflows/ci.yml)

---

## 特性

- **零依赖**：仅使用 C 标准库
- **跨平台**：Linux / Windows / macOS，自动平台检测
- **多结果集**：单包支持多个结果集（ANSWER 包）
- **转义机制**：二进制安全，支持任意字节内容
- **CRC32**：IEEE 802.3 标准校验
- **线程安全**：同一实例禁止并发写，finalized 包可并发读
- **多语言 FFI**：提供 C / C++ / Python / Rust 示例

---

## 快速开始

### 编译

```bash
# Linux / macOS
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON -DBUILD_DEMOS=ON
make -j4

# Windows (MSYS2 / MinGW)
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DBUILD_TESTS=ON -DBUILD_DEMOS=ON
mingw32-make -j4
```

### 运行测试

```bash
./bin/Lnx64/test_msgpacket    # 单元测试
./bin/Lnx64/test_regression  # 回归测试
./bin/Lnx64/test_fuzz        # 模糊测试
./bin/Lnx64/test_perf        # 性能测试

# ASAN 验证（构建时启用）
cmake .. -DENABLE_ASAN=ON -DENABLE_LSAN=ON
make -j4
ctest --output-on-failure
```

### 运行 Demo

```bash
./bin/Lnx64/demo_builder     # 构建数据包
./bin/Lnx64/demo_parser     # 解析数据包
./bin/Lnx64/demo_full_cycle # 完整收发周期
```

---

## 项目结构

```
msgpacket/
├── SPEC.md              协议规范文档
├── README.md            本文件
├── CMakeLists.txt       构建配置
│
├── src/                 核心源码
│   ├── msg_packet.h     协议结构体（#pragma pack）
│   ├── msg_api.c/h      统一 API（创建/构建/解析/finalize）
│   ├── msg_util.c/h     UUID v4 / CRC32 / 转义工具
│   ├── msg_byteorder.h  字节序转换
│   ├── msg_build.c      构建器实现
│   └── msg_query.c      查询解析实现
│
├── tests/
│   ├── minunit.h        零依赖测试框架
│   ├── test_msgpacket.c 单元测试（273 项）
│   ├── test_regression.c 回归测试（135 项）
│   ├── test_fuzz.c      模糊测试（49 项）
│   └── test_perf.c      性能测试（15022 项）
│
├── demo/                多语言 FFI 示例
│   ├── c/               C 示例
│   ├── cpp/             C++ RAII 示例
│   ├── python/          Python ctypes 示例
│   └── rust/            Rust FFI 示例
│
└── library/             安装产物目录
    ├── include/         头文件
    ├── lib/             静态库
    └── bin/             动态库
```

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

完整 API 文档见 [SPEC.md](SPEC.md)。

---

## Wire 布局

```
偏移  0 : magic[4]     = "YSWY"
偏移  4 : crc32[4]     = 小端序
偏移  8 : body_len[4]  = 小端序
偏移 12 : header       = msg_header_t (72 字节)
偏移 83 : body[]       = 柔性数组
总长    : 83 + body_len
```

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

## 性能基准

```
CPU: Intel i7-12700K
OS:  Linux (Ubuntu 22.04)

编码吞吐：  ~1.2 GB/s
解码吞吐：  ~0.9 GB/s
内存占用：  < 1 KB per packet
```

---

## 许可证

MIT License
