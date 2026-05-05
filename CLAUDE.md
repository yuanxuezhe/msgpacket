# MsgPacket 项目知识库

项目协议规范详见 `SPEC.md`。

---

## 项目结构

```
msgpacket/
├── SPEC.md               协议规范文档
├── CLAUDE.md             本文件，项目知识库
├── CMakeLists.txt        CMake 编译配置
│
├── src/                  C 语言 API 源码
│   ├── msg_byteorder.h  字节序转换
│   ├── msg_packet.h     协议结构体定义（packed）
│   ├── msg_util.c/h     UUID/CRC32/转义工具
│   └── msg_api.c/h      统一 API
│
├── library/              编译产物（运行时库）
│   ├── include/         公开头文件
│   ├── bin/x64/         动态库 (.so/.dll)
│   └── lib/x64/         静态库 (.a)
│
├── tests/
│   └── test_msgpacket.c Unity 测试用例
│
└── demo/                 多语言 FFI 示例
    ├── c/               C demo
    ├── cpp/             C++ RAII demo
    ├── python/          Python ctypes demo
    └── rust/            Rust FFI demo
```

---

## 编译

```bash
mkdir build && cd build
cmake .. && make -j4
./bin/Lnx64/test_msgpacket
```

---

## 已知限制

1. **msg_set_row 使用逗号分隔**：内部用逗号作为列分隔符，`msg_finalize()` 将逗号替换为 US (0x1F) 写入 wire
2. **行解析兼容逗号和 US**：同时支持两种分隔符
3. **timestamp 无 \0 终止**：固定 17 字节 `yyyyMMddHHmmssSSS`
4. **body_len 和 crc32 使用小端序**：其他字段为 ASCII/字节数组，无需字节序转换
