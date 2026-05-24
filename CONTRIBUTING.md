# 贡献指南

感谢您对 MsgPacket 项目的兴趣！我们欢迎各种形式的贡献，包括代码改进、bug 修复、文档完善和测试补充。

---

## 开发环境

### 环境要求

- **C 编译器**：支持 C99（如 `gcc`、`clang`、`msvc`）
- **CMake**：≥ 3.15
- **操作系统**：Linux / Windows (MSYS2/MinGW) / macOS

### 快速编译

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
# 全部测试
ctest --output-on-failure

# 单项测试
./bin/Lnx64/test_msgpacket    # 单元测试（273 项）
./bin/Lnx64/test_regression  # 回归测试（135 项）
./bin/Lnx64/test_fuzz        # 模糊测试（49 项）
./bin/Lnx64/test_perf        # 性能测试（15022 项）

# ASAN 验证
cmake .. -DENABLE_ASAN=ON -DENABLE_LSAN=ON
make -j4
ctest --output-on-failure
```

---

## 开发流程

### 1. Fork & Clone

```bash
git clone https://github.com/<your-username>/msgpacket.git
cd msgpacket
git remote add upstream https://github.com/msgpacket/msgpacket.git
```

### 2. 创建功能分支

```bash
git checkout -b feat/my-new-feature    # 新功能
git checkout -b fix/my-bug-fix         # Bug 修复
git checkout -b docs/my-doc-update    # 文档更新
```

### 3. 开发与测试

- 编写代码，确保所有新增函数有单元测试
- 运行 `ctest --output-on-failure` 确保全部测试通过
- 启用 ASAN 检查内存问题：`cmake .. -DENABLE_ASAN=ON`

### 4. 提交代码

```bash
git add .
git commit -m "<type>: <subject>"
```

**提交信息格式**：

| Type | 说明 |
|------|------|
| `feat` | 新功能 |
| `fix` | Bug 修复 |
| `docs` | 文档更新 |
| `style` | 代码格式（不影响功能） |
| `refactor` | 重构（不影响功能） |
| `test` | 测试相关 |
| `chore` | 构建/工具变更 |

**示例**：

```
feat: 添加 msg_get_result_set_count API
fix: 修复字段值含逗号时数据截断问题
docs: 更新 SPEC.md 协议规范
test: 添加多结果集回归测试
refactor: 拆分 msg_api.c 为 msg_build 和 msg_query
```

### 5. Push & Pull Request

```bash
git push origin feat/my-new-feature
```

然后在 GitHub 上创建 Pull Request，描述改动内容和关联的 issue。

---

## 代码规范

### 缩进与格式

- **缩进**：4 空格（禁止 tab）
- **行宽**：单行不超过 120 字符
- **大括号**：K&R 风格（`if () {\n`）

### 命名约定

| 类型 | 风格 | 示例 |
|------|------|------|
| 变量 / 函数 | `snake_case` | `msg_create`, `body_len` |
| 结构体 | `snake_case_t` | `msg_packet_t` |
| 宏定义 | `UPPER_SNAKE_CASE` | `MSG_MAGIC`, `MSG_MAX_ROWS` |
| 头文件保护 | `MSG_<FILE>_H` | `MSG_API_H` |

### 注释规范

- 复杂逻辑必须注释，说明 **为什么** 而非 **是什么**
- 使用 `/** ... */` 描述公共 API 的参数和返回值
- 单行注释使用 `//`（C99 支持）

### 函数规范

```c
/**
 * 简短描述。
 *
 * 参数:
 *   packet  - 数据包实例
 *   key     - 字段键名
 *   value   - 字段值
 *
 * 返回:
 *   成功返回 0，失败返回负错误码
 *
 * 注意:
 *   此函数非线程安全，同一实例不可并发调用
 */
int msg_set_value_str(msg_packet_t *packet, const char *key, const char *value);
```

### 错误处理

- 所有返回 `int` 的 API 必须检查返回值
- 错误码使用 `MSG_ERR_*` 系列宏
- 禁止裸露的 `if (ret < 0) return ret;` 无注释

---

## 测试要求

### 测试框架

项目使用 **MinUnit**（header-only 零依赖测试框架），位于 `tests/minunit.h`。

### 测试文件结构

| 文件 | 说明 |
|------|------|
| `tests/minunit.h` | 测试框架头文件 |
| `tests/test_msgpacket.c` | 单元测试（273 项） |
| `tests/test_regression.c` | 回归测试（135 项） |
| `tests/test_fuzz.c` | 模糊测试（49 项） |
| `tests/test_perf.c` | 性能测试（15022 项） |
| `tests/test_full_cycle.c` | 完整周期测试 |

### 测试覆盖要求

- 所有新增公共 API 必须有对应的单元测试
- Bug 修复必须附带回归测试
- 测试函数命名：`test_<功能>_<场景>`

### 测试通过标准

- `ctest --output-on-failure` 必须全部通过
- 启用 ASAN 构建后无内存泄漏或越界访问

---

## 项目结构

```
msgpacket/
├── SPEC.md              协议规范文档
├── CLAUDE.md            项目知识库
├── README.md            项目说明
├── CHANGELOG.md         版本变更记录
├── CONTRIBUTING.md      本贡献指南
├── CMakeLists.txt       构建配置
│
├── src/                 核心源码
│   ├── msg_packet.h     协议结构体（#pragma pack）
│   ├── msg_api.c/h      统一 API
│   ├── msg_util.c/h     UUID v4 / CRC32 / 转义工具
│   ├── msg_byteorder.h  字节序转换
│   ├── msg_build.c/h    构建器实现
│   ├── msg_query.c/h    查询解析实现
│   └── msg_internal.h   内部状态管理
│
├── tests/               测试代码
│   ├── minunit.h        零依赖测试框架
│   ├── test_msgpacket.c 单元测试
│   ├── test_regression.c 回归测试
│   ├── test_fuzz.c      模糊测试
│   ├── test_perf.c      性能测试
│   └── test_full_cycle.c 完整周期测试
│
└── demo/                 多语言 FFI 示例
    ├── c/               C 示例
    ├── cpp/             C++ RAII 示例
    ├── python/          Python ctypes 示例
    └── rust/            Rust FFI 示例
```

---

## 协议实现注意事项

### 字节序

- 仅 `body_len` 和 `crc32`（两个 `uint32_t`）需要小端序转换
- 其他字段为 ASCII/字节数组，无需转换

### 线程安全

1. 同一 `msg_packet_t` 实例**不可**并发调用修改操作
2. 已 `finalized` 的包可**多线程并发读取**
3. 并发写入需用 `msg_clone()` 创建独立副本

### CRC32 计算

- **多项式**：`0xEDB88320`（IEEE 802.3 标准）
- **初始值**：`0xFFFFFFFF`，最终异或 `0xFFFFFFFF`
- **计算范围**：`body_len(4) + header(72) + body(n)`，不含 magic(4) 和 crc32(4) 自身

### 转义规则

| 原始字节 | 编码后 |
|----------|--------|
| `0x1F` (US) | `0x1B 0x5F` |
| `0x1E` (RS) | `0x1B 0x5E` |
| `0x1C` (FS) | `0x1B 0x5C` |
| `0x1B` (ESC) | `0x1B 0x5B` |
| `0x1D` (GS) | `0x1B 0x5D` |

---

## 许可证

MsgPacket 采用 MIT 许可证。贡献的代码默认以 MIT 许可证发布。

---

## 问题与反馈

- **Bug 报告**：请创建 GitHub Issue，附上复现步骤
- **功能请求**：请创建 GitHub Issue，描述需求场景
- **Pull Request**：所有 PR 需通过 CI 测试，我们会尽快 review