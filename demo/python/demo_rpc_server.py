#!/usr/bin/env python3
"""
Demo2: RPC Server - 从队列1获取请求，处理后返回应答到队列2

用法:
    python demo_rpc_server.py

依赖:
    pip install aio-pika
"""

import asyncio
import json
from datetime import datetime

import aio_pika
from aio_pika import ExchangeType

from msgpacket import MsgPacket, MSG_TYPE_ANSWER


RABBITMQ_URL = "amqp://192.168.10.2:5672/"
EXCHANGE_NAME = "msgpacket.exchange"
QUEUE_REQ = "EvTrade.Req"      # 队列1：获取请求
QUEUE_REPLY = "EvTrade.Reply"   # 队列2：发送应答


def handle_request(pkt: MsgPacket) -> dict:
    """处理请求并返回结果"""
    func = pkt.func().strip('\x00')

    if func == "echo":
        seq = pkt.get_value_str("seq")
        data = pkt.get_value_str("data")
        return {"code": "00000", "result": f"processed: {data} (seq={seq})"}
    elif func == "ping":
        return {
            "code": "00000",
            "pong": True,
            "server_time": datetime.now().strftime('%Y-%m-%d %H:%M:%S.%f')[:-3],
        }
    else:
        return {"code": "99999", "error": f"unknown func: {func}"}


async def on_message(message: aio_pika.IncomingMessage, channel: aio_pika.Channel):
    """处理每条到达的消息 - message.process() 自动 ack"""
    async with message.process():
        wire_data = message.body

        try:
            pkt = MsgPacket.decode(wire_data)
        except RuntimeError as e:
            print(f"[Server] decode error: {e}")
            return

        req_msg_id = pkt.msg_id().strip()
        req_func = pkt.func().strip('\x00')
        print(f"[Server] <- request: msg_id={req_msg_id}, "
              f"func={req_func}, size={len(wire_data)}")

        # 处理请求
        result = handle_request(pkt)
        print(f"[Server] handle -> {result}")

        # 构建应答包
        ts = datetime.now().strftime('%Y%m%d%H%M%S') + '000'
        ans = MsgPacket(MSG_TYPE_ANSWER, pkt.version())
        ans.set_msg_id(req_msg_id)
        ans.set_timestamp(ts)
        ans.set_func(req_func)
        ans.set_headers(2, "code,message")
        ans.add_row()
        ans.set_value("code", result["code"])
        ans.set_value("message", json.dumps(result, ensure_ascii=False))
        ans.finalize()

        _, ans_wire = ans.encode()

        # 发送到队列2（应答）
        await channel.default_exchange.publish(
            aio_pika.Message(body=ans_wire),
            routing_key=QUEUE_REPLY,
        )
        print(f"[Server] -> reply: {len(ans_wire)} bytes "
              f"to [{QUEUE_REPLY}], msg_id={req_msg_id}")


async def main():
    conn = await aio_pika.connect_robust(RABBITMQ_URL)
    print(f"[Server] Connected: {RABBITMQ_URL}")

    async with conn:
        channel = await conn.channel()
        await channel.set_qos(prefetch_count=10)

        exchange = await channel.declare_exchange(
            EXCHANGE_NAME, ExchangeType.TOPIC, durable=True,
        )

        # 声明并绑定队列1（接收请求）
        req_queue = await channel.declare_queue(QUEUE_REQ, durable=True)
        await req_queue.bind(exchange, routing_key=QUEUE_REQ)
        print(f"[Server] Listening on [{QUEUE_REQ}] ...")

        # 使用 consume 方式接收消息，避免 iterator 的跳读问题
        await req_queue.consume(lambda msg: on_message(msg, channel))


if __name__ == "__main__":
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        loop.run_until_complete(main())
    except KeyboardInterrupt:
        print("\n[Server] Exited")
    finally:
        loop.close()