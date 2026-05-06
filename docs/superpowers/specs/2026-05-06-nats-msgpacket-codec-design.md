# NATS Demo MsgPacket 编解码集成

## 目标

`publisher_demo.py` 和 `subscriber_demo.py` 增加 MsgPacket 编解码逻辑，参考 `kafka_producer_demo.py` 和 `kafka_consumer_demo.py` 的模式。

## 设计

### publisher_demo.py

**当前**: 发 5 条纯 JSON 字符串  
**改为**: 构建 MsgPacket kline 消息 → finalize → encode → 发 wire bytes

流程:
1. 导入 `MsgPacket`, `MSG_TYPE_PUSH` from msgpacket
2. 创建 kline 消息（Symbol,Price,Volume,Timestamp）
3. `finalize()` + `encode()` 生成 wire bytes
4. `nc.publish()` 发送 wire bytes
5. 单条消息，发完退出

参照: `kafka_producer_demo.py` 的 `build_kline_msg()`

### subscriber_demo.py

**当前**: `msg.data.decode("utf-8")` 直接打印  
**改为**: `MsgPacket.decode(msg.data)` → 打印解码详情

流程:
1. 导入 `MsgPacket` from msgpacket
2. 收到消息后 `MsgPacket.decode(msg.data)` 解码
3. 打印 func, msg_type, version, timestamp, headers, rows
4. 遍历行列打印字段值
5. 保持无限阻塞 + Ctrl+C 退出

参照: `kafka_consumer_demo.py` 的 `decode_and_print()`

### 不改动

- NATS 连接配置
- Stream/Subject 名称
- 阻塞等待逻辑
