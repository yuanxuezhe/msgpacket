# QMT + NATS 发布订阅整合

## 目标

将 QMT 策略 demo 与 NATS JetStream 发布订阅整合：QMT 回调接收交易事件 → MsgPacket 编码 → 发布到 NATS → 外围 subscriber 消费。

## 架构

```
主线程 (asyncio):  NATS 连接 + 永久阻塞 (Ctrl+C 退出)
后台线程 (daemon): xt_trader.run_forever()
QMT 回调:          asyncio.run_coroutine_threadsafe(publish, main_loop)
```

## 新增文件

### `demo/python/qmt_publisher_demo.py`

组件:
- `NatsPublisher`: 封装 NATS 连接、JetStream context、MsgPacket 构建与发布
- `QmtCallback`: 继承 `XtQuantTraderCallback`，持有 `NatsPublisher` 和 main loop 引用
- `main()`: 创建 NATS 连接 → 启动 QMT 后台线程 → asyncio 永久阻塞

### Subject 设计

单 subject `qmt.events`，stream `QMT_EVENTS`，用 `func` 区分事件类型:

| 回调 | func | headers |
|------|------|---------|
| on_stock_order | stock_order | StockCode,AccountID,OrderID,OrderSysID,OrderStatus,OrderVolume,TradedVolume |
| on_stock_trade | stock_trade | StockCode,AccountID,OrderID,OrderSysID,TradedID,TradedVolume,Direction |
| on_order_error | order_error | OrderID,ErrorMsg,OrderRemark |
| on_cancel_error | cancel_error | OrderID,ErrorMsg,Market |
| on_account_status | account_status | AccountID,Status |
| on_disconnected | disconnected | (空行，仅信号) |

### Subscriber 改动

`subscriber_demo.py` 只需改 subject 为 `qmt.events`，stream 为 `QMT_EVENTS`。
