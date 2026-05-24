# Changelog

All notable changes to the **MsgPacket** project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，and this project adheres to [Semantic Versioning](https://semver.org/).

---

## [1.1.0] — 2026-05-24

### Added
- **多结果集支持（ANSWER 包）**：新增 `msg_add_result_set` / `msg_next_result_set` / `msg_select_result_set` / `msg_get_result_set_count` API，支持单个数据包包含多个结果集，结果集间以 GS (0x1D) 分隔
- **性能测试**：新增 `test_perf.c`，包含 15022 项性能测试用例，编码吞吐 ~1.2 GB/s，解码吞吐 ~0.9 GB/s
- **模糊测试**：新增 `test_fuzz.c`，包含 49 项模糊测试用例
- **回归测试**：新增 `test_regression.c`，包含 135 项回归测试
- **ASAN / LSAN 支持**：CMake 支持 `-DENABLE_ASAN=ON -DENABLE_LSAN=ON` 启用内存 sanitizer 验证
- **Python / Rust FFI Demo**：新增 `demo/python/`（ctypes）和 `demo/rust/`（FFI）示例代码
- **Rust RPC Demo**：新增 Rust + RabbitMQ RPC 示例
- **Kafka Demo**：新增 Kafka producer/consumer 示例
- **C++ RAII Demo**：新增 `demo/cpp/`，包含 `msg_packet.hpp` 封装
- **UUID v4 msg_id**：自动生成 32 字节 UUID 作为消息唯一标识
- **自动时间戳**：调用 `msg_create` 时自动生成 `yyyyMMddHHmmssSSS` 格式时间戳
- **自动表头增长**：未显式设置表头时，自动为每个字段分配列名（如 `col_0`、`col_1`）
- **线程安全规则**：明确同一实例不可并发写，finalized 包可并发读

### Changed
- **重构代码结构**：`msg_api.c` 拆分为 `msg_build.c`（构建器）和 `msg_query.c`（查询解析），`msg_internal.h` 管理内部状态
- **内部存储优化**：统一使用 `char***` 存储字段值，移除 `field_desc_t` 冗余逻辑
- **转义逻辑优化**：字段值含逗号时不再截断，支持任意字节内容
- **原子操作改进**：使用 C11 原子操作替代互斥锁
- **字符串解析替换**：`strtol`/`strtoll` 替换 `atoi`，带错误处理
- **isprint 修复**：转义时正确过滤不可打印字符
- **CRC32 优化**：5 项性能优化
- **移除 cmocka 依赖**：测试框架替换为零依赖的 MinUnit（头文件-only）

### Fixed
- **header 解析 bug**：修复 header 字段解析错误
- **字段值含逗号截断 bug**：`msg_set_row` 遇逗号截断的问题
- **arc4random_buf 替换 getrandom**：Linux 环境下 `getrandom` 需要 `_GNU_SOURCE`，改用 `arc4random_buf` 跨平台兼容
- **internal_find_col 匹配逻辑**：修复列查找匹配错误
- **finalized 参数冗余**：移除不必要的 finalized 参数

### Docs
- 新增 `SPEC.md` 协议规范文档
- 新增 `CLAUDE.md` 项目知识库
- 更新 `.gitignore` 并移除已跟踪的二进制文件

---

## [1.0.0] — 2026-05-06

### Added
- **MsgPacket 协议实现**：纯 C99 实现，零外部依赖
- **核心数据结构**：`msg_packet_t` / `msg_header_t`，固定头部 + 表格式 Body
- **协议特性**：
  - 固定魔数 `"YSWY"`
  - IEEE 802.3 CRC32 校验
  - ASCII 分级分隔符体系（GS/RS/FS/US）
  - 二进制转义机制
  - 多字段类型支持（字符串、i32、i64、double）
- **基础 API**：
  - 创建/销毁/克隆：`msg_create` / `msg_destroy` / `msg_clone`
  - Header 设置：`msg_set_msg_id` / `msg_set_func` / `msg_set_timestamp` / `msg_set_version`
  - 表头构建：`msg_set_headers` / `msg_add_header`
  - 数据行操作：`msg_add_row` / `msg_set_row` / `msg_set_value_str/i32/i64/double`
  - 提交与获取：`msg_finalize` / `msg_data` / `msg_size`
  - 编码/解码：`msg_encode` / `msg_decode` / `msg_free_buffer`
  - 数据遍历：`msg_fetch_next` / `msg_reset_cursor` / `msg_get_current_row`
  - 字段获取：`msg_get_value_str/i32/i64/double` / `msg_get_field`
- **错误码体系**：15 个错误码（`MSG_ERR_NULL_PTR` ~ `MSG_ERR_NOT_FINALIZED`）
- **跨平台支持**：Linux / Windows / macOS 自动检测
- **CMake 构建系统**：支持 `BUILD_TESTS=ON` / `BUILD_DEMOS=ON`
- **C Demo**：构建器、解析器、完整收发周期示例
- **基础单元测试**：MinUnit 框架，273 项测试用例