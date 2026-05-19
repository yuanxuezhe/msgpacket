#!/usr/bin/env python3
"""
RPC Server Demo - 基于 msgid 的请求-应答匹配

收到请求后，提取 msgid，在应答中原样返回。
使用 func 字段区分不同服务（getData / calc）。
"""

import asyncio
import uuid
import json
from datetime import datetime

import aio_pika
from aio_pika import ExchangeType

from msgpacket import MsgPacket, MSG_TYPE_REQUEST, MSG_TYPE_ANSWER


RABBITMQ_URL = "amqp://192.168.10.2:5672/"
EXCHANGE_NAME = "msgpacket.exchange"
QUEUE_REQ = "EvTrade.Req"


# 业务处理函数
def handle_request(pkt: MsgPacket) -> dict:
    """根据 func 字段分发到不同的处理函数"""
    func = pkt.func().strip('\x00')  # 去除可能的尾部垃圾

    if func == "getData":
        return {
            "Symbol": "BTC/USDT",
            "Price": 42150.5,
            "Volume": 123.456,
            "Time": "20250101123045"
        }
    elif func == "calc":
        # 从请求中取两个数做加法
        try:
            a = float(pkt.get_value_str("a"))
            b = float(pkt.get_value_str("b"))
            return {"result": a + b}
        except Exception:
            return {"error": "invalid parameters"}
    elif func == "echo":
        # 简单回显
        headers = pkt.get_headers()
        row_count = pkt.row_count()
        return {
            "echo": "ok",
            "headers": headers,
            "rows": row_count
        }
    elif func == "ping":
        # 心跳 ping，返回当前服务器时间戳
        from datetime import datetime
        now = datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3]
        return {
            "pong": True,
            "server_time": now,
            "msg_id": pkt.msg_id()
        }
    else:
        return {"error": f"unknown func: {func}"}


async def main():
    conn = await aio_pika.connect_robust(RABBITMQ_URL)
    print(f"[Server] Connected: {RABBITMQ_URL}")

    async with conn:
        channel = await conn.channel()
        await channel.set_qos(prefetch_count=10)
        exchange = await channel.declare_exchange(
            EXCHANGE_NAME, ExchangeType.TOPIC, durable=True,
        )

        req_queue = await channel.declare_queue(QUEUE_REQ, durable=True)
        await req_queue.bind(exchange, routing_key=QUEUE_REQ)
        print(f"[Server] Listening on [{QUEUE_REQ}] ...")

        async with req_queue.iterator() as qiter:
            async for msg in qiter:
                wire_data = msg.body
                reply_to = msg.reply_to  # 请求方指定的应答队列

                try:
                    pkt = MsgPacket.decode(wire_data)
                except RuntimeError as e:
                    print(f"  [Decode Error] {e}")
                    await msg.ack()
                    continue

                req_msg_id = pkt.msg_id()
                req_func = pkt.func().strip('\x00')
                print(f"  [Server] recv msg_id={req_msg_id}, func={req_func}, "
                      f"reply_to={reply_to}, size={len(wire_data)}")

                # 处理请求
                result = handle_request(pkt)
                print(f"  [Server] handle -> {result}")

                # 构建应答消息（先 set_timestamp，再 set_func，避免 C 库 bug 破坏 func）
                ts = datetime.now().strftime('%Y%m%d%H%M%S') + '000'
                ans = MsgPacket(MSG_TYPE_ANSWER, pkt.version())
                ans.set_msg_id(req_msg_id)  # ← 关键：用同一个 msgid
                ans.set_timestamp(ts)
                ans.set_func(req_func)
                ans.set_headers(2, "code,message")
                ans.add_row()
                ans.set_value("code", "00000")
                ans.set_value("message", json.dumps(result, ensure_ascii=False))
                ans.finalize()

                _, ans_wire = ans.encode()

                # 发送到请求方指定的 reply_to 队列
                if reply_to:
                    await channel.default_exchange.publish(
                        aio_pika.Message(
                            body=ans_wire,
                            correlation_id=req_msg_id,  # RabbitMQ 自带的 corr_id（可选）
                        ),
                        routing_key=reply_to,
                    )
                    print(f"  [Server] replied {len(ans_wire)} bytes -> [{reply_to}], "
                          f"msg_id={req_msg_id}")
                else:
                    print(f"  [Server] no reply_to, discarded")

                await msg.ack()


if __name__ == "__main__":
    try:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        loop.run_until_complete(main())
    except KeyboardInterrupt:
        print("\n[Server] Exiting...")
    finally:
        loop.close()
