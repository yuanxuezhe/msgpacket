#!/usr/bin/env python3
"""
RPC Client Demo - 基于 msgid 的请求-应答匹配

发送请求后，通过 asyncio.Future + msgid 等待并匹配应答。
每个请求生成唯一 msgid，应答中携带相同 msgid 用于匹配。
"""

import asyncio
import uuid
import json
from concurrent.futures import TimeoutError as FutTimeoutError

import aio_pika
from aio_pika import ExchangeType, Message

from msgpacket import MsgPacket, MSG_TYPE_REQUEST


RABBITMQ_URL = "amqp://192.168.10.2:5672/"
EXCHANGE_NAME = "msgpacket.exchange"
QUEUE_REQ = "EvTrade.Req"


class RpcClient:
    """
    基于 msgid 匹配的 RPC 客户端。

    用法:
        client = RpcClient(rabbitmq_url)
        await client.connect()

        # 调用远程服务，像本地函数一样用
        result = await client.call("getData", {})
        result = await client.call("calc", {"a": 1, "b": 2})

        await client.close()
    """

    def __init__(self, url: str, reply_queue_name: str = None):
        self.url = url
        # 每个客户端实例有独立的临时应答队列，通过 reply_to 接收应答
        self.reply_queue_name = reply_queue_name or f"rpc.reply.{uuid.uuid4().hex[:8]}"
        self.conn = None
        self.channel = None
        self.reply_queue = None
        self.exchange = None

        # pending[msgid] = asyncio.Future
        self._pending: dict[str, asyncio.Future] = {}
        self._reply_task = None

    async def connect(self):
        self.conn = await aio_pika.connect_robust(self.url)
        self.channel = await self.conn.channel()

        # 声明应答队列（临时队列，exclusive）
        self.reply_queue = await self.channel.declare_queue(
            self.reply_queue_name,
            durable=False,      # 临时队列，关闭后自动删除
            exclusive=True,      # 仅本连接可读
        )
        print(f"[Client] Connected, reply_queue={self.reply_queue_name}")

        # 启动应答分发协程
        self._reply_task = asyncio.create_task(self._dispatch_reply())

    async def _dispatch_reply(self):
        """监听应答队列，根据 msgid 分发到对应的 Future"""
        async with self.reply_queue.iterator() as qiter:
            async for msg in qiter:
                corr_id = msg.correlation_id  # RabbitMQ 的 correlation_id
                wire_data = msg.body

                try:
                    pkt = MsgPacket.decode(wire_data)
                except RuntimeError as e:
                    print(f"  [Client] decode error: {e}")
                    await msg.ack()
                    continue

                msg_id = pkt.msg_id()

                # 用 msgid 匹配（主要），fallback 到 corr_id
                key = msg_id or corr_id
                if key and key in self._pending:
                    future = self._pending.pop(key)
                    if not future.done():
                        future.set_result(pkt)
                    print(f"  [Client] matched msg_id={msg_id}, pending={len(self._pending)}")
                else:
                    # 没有匹配的 pending，说明是过期的应答或其他人发来的
                    print(f"  [Client] unexpected reply msg_id={msg_id}, "
                          f"corr_id={corr_id}, pending_keys={list(self._pending.keys())}")

                await msg.ack()

    async def call(self, func: str, params: dict, timeout: float = 30.0) -> dict:
        """
        发起 RPC 调用，等待应答并返回结果。

        参数:
            func: 服务端的方法名（如 "getData", "calc"）
            params: 方法参数（key-value）
            timeout: 超时秒数

        返回:
            dict: 服务端返回的结果

        异常:
            TimeoutError: 应答超时
            RuntimeError: 编码/解码错误
        """
        if not self.channel:
            raise RuntimeError("Not connected, call connect() first")

        # 生成唯一 msgid（只用 hex 部分，32字符，适配 HEAD_MSGID_LENGTH=32）
        msg_id = uuid.uuid4().hex

        # 创建 Future 并注册到 pending
        future: asyncio.Future = asyncio.get_event_loop().create_future()
        self._pending[msg_id] = future

        try:
            # 构建请求（先 set_timestamp，再 set_func，避免 C 库 bug 破坏 func）
            from datetime import datetime
            ts = datetime.now().strftime('%Y%m%d%H%M%S') + '000'
            pkt = MsgPacket(MSG_TYPE_REQUEST, "V1.0")
            pkt.set_msg_id(msg_id)
            pkt.set_timestamp(ts)
            pkt.set_func(func)

            # 设置表头和参数
            if params:
                headers = ",".join(params.keys())
                pkt.set_headers(len(params), headers)
                pkt.add_row()
                for k, v in params.items():
                    pkt.set_value(k, v)

            pkt.finalize()
            _, wire_data = pkt.encode()

            # 发送到请求队列，reply_to 指向本客户端的应答队列
            await self.exchange.publish(
                Message(
                    body=wire_data,
                    reply_to=self.reply_queue_name,     # 告诉服务端应答发到哪
                    correlation_id=msg_id,                # RabbitMQ 层面的匹配 id
                ),
                routing_key=QUEUE_REQ,
            )
            print(f"  [Client] sent msg_id={msg_id}, func={func}, params={params}, "
                  f"reply_to={self.reply_queue_name}, size={len(wire_data)}")

            # 等待应答
            try:
                ans_pkt: MsgPacket = await asyncio.wait_for(future, timeout=timeout)
            except asyncio.TimeoutError:
                self._pending.pop(msg_id, None)
                raise TimeoutError(f"RPC call {func} timeout after {timeout}s (msg_id={msg_id})")

            # 解析应答内容
            code = ans_pkt.get_value_str("code")
            message = ans_pkt.get_value_str("message")

            if code != "00000":
                return {"error": f"code={code}", "message": message}

            return json.loads(message) if message else {}

        finally:
            # 确保从 pending 中移除（正常情况下在上面已 pop）
            self._pending.pop(msg_id, None)

    async def close(self):
        if self._reply_task:
            self._reply_task.cancel()
            try:
                await self._reply_task
            except asyncio.CancelledError:
                pass
        if self.conn:
            await self.conn.close()
        print("[Client] Closed")


async def demo_calls(client: RpcClient):
    """演示各种 RPC 调用"""

    # 1. getData - 获取行情数据
    print("\n=== Call: getData ===")
    result = await client.call("getData", {})
    print(f"Result: {result}")

    # 2. calc - 计算
    print("\n=== Call: calc (1 + 2) ===")
    result = await client.call("calc", {"a": 1, "b": 2})
    print(f"Result: {result}")

    # 3. echo - 回显
    print("\n=== Call: echo ===")
    result = await client.call("echo", {"key": "value"})
    print(f"Result: {result}")

    # 4. calc - 浮点数
    print("\n=== Call: calc (10.5 + 20.3) ===")
    result = await client.call("calc", {"a": 10.5, "b": 20.3})
    print(f"Result: {result}")

    # 5. 超时演示
    print("\n=== Call: timeout (1s) ===")
    try:
        result = await client.call("getData", {}, timeout=1.0)
        print(f"Result: {result}")
    except TimeoutError as e:
        print(f"TimeoutError: {e}")

    # 6. ping - 心跳
    print("\n=== Call: ping ===")
    result = await client.call("ping", {})
    print(f"Result: {result}")


async def main():
    client = RpcClient(RABBITMQ_URL)

    try:
        await client.connect()

        # 先声明 exchange（connect 后再声明，因为需要 channel）
        client.exchange = await client.channel.declare_exchange(
            EXCHANGE_NAME, ExchangeType.TOPIC, durable=True,
        )

        await demo_calls(client)

    except Exception as e:
        print(f"[Error] {e}")
    finally:
        await client.close()


if __name__ == "__main__":
    try:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        loop.run_until_complete(main())
    except KeyboardInterrupt:
        print("\n[Client] Interrupted")
    finally:
        loop.close()
