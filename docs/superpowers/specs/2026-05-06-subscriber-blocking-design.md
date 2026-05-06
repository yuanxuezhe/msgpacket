# Subscriber Demo 阻塞优化

## 目标

优化 `demo/python/subscriber_demo.py`，使其持续阻塞运行，收到 Ctrl+C 信号时才优雅退出。

## 当前问题

`next_msg(timeout=5)` 的 `TimeoutError` 被 catch 在 `while True` 循环外部，导致超时后直接走到 `finally` 退出，无法持续运行。"retrying" 日志实际上不会触发重试。

## 设计

### 改动点

1. **无限阻塞等待**: `next_msg(timeout=None)` 替代 `next_msg(timeout=5)`，有消息时立即返回，无消息时永久阻塞
2. **移除超时异常处理**: 删除 `except nats.errors.TimeoutError` 分支（不再超时）
3. **保留异常处理**:
   - `asyncio.CancelledError`: asyncio 任务取消
   - `KeyboardInterrupt` (外层): Ctrl+C 优雅退出
   - `finally`: 确保 `unsubscribe()` + `close()` 始终执行

### 退出路径

| 触发方式 | 处理机制 | 清理 |
|---------|---------|------|
| Ctrl+C | `KeyboardInterrupt` → 外层 catch | finally 块 |
| asyncio 取消 | `asyncio.CancelledError` | finally 块 |
| NATS 连接断开 | 异常传播 | finally 块 |

### 不改动

- 不引入 signal 模块（保持简洁）
- 不添加重连逻辑（超出需求范围）
- 不修改 NATS 连接配置
