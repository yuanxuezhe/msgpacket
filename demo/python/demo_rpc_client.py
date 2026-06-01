#!/usr/bin/env python3
"""
Demo1: RPC Client - 发送请求到队列1，从队列2接收应答

用法:
    python demo_rpc_client.py

依赖:
    pip install aio-pika
"""

import asyncio
import uuid
from datetime import datetime

import aio_pika
from aio_pika import ExchangeType, Message

from msgpacket import MsgPacket, MSG_TYPE_REQUEST


RABBITMQ_URL = "amqp://192.168.10.2:5672/"
EXCHANGE_NAME = "msgpacket.exchange"
QUEUE_REQ = "EvTrade.Req"      # 队列1：发送请求
QUEUE_REPLY = "EvTrade.Reply"   # 队列2：接收应答


# 用于在 on_message 中传递回复信息
reply_results = {}


async def on_reply(message: aio_pika.IncomingMessage):
    """处理每条到达的回复消息 - message.process() 自动 ack"""
    async with message.process():
        wire_data = message.body
        try:
            pkt = MsgPacket.decode(wire_data)
            msg_id = pkt.msg_id().strip()
            func = pkt.func().strip('\x00')
            code = pkt.get_value_str("code")
            message_text = pkt.get_value_str("message")
            reply_results[msg_id] = (func, code, message_text)
            print(f"[Client] <- reply: msg_id={msg_id}, func={func}, "
                  f"code={code}, message={message_text}")
        except Exception as e:
            print(f"[Client] decode error: {e}")


async def main():
    # 连接 RabbitMQ
    conn = await aio_pika.connect_robust(RABBITMQ_URL)
    channel = await conn.channel()

    # 声明 exchange 和队列
    exchange = await channel.declare_exchange(
        EXCHANGE_NAME, ExchangeType.TOPIC, durable=True,
    )
    await channel.declare_queue(QUEUE_REQ, durable=True)
    reply_queue = await channel.declare_queue(QUEUE_REPLY, durable=True)
    print(f"[Client] Connected, request -> [{QUEUE_REQ}], reply from [{QUEUE_REPLY}]")

    # 启动回复监听协程
    await reply_queue.consume(on_reply)

    try:
        # 发送 5 个请求到队列1
        for i in range(5):
            await send_request(channel, exchange, i)
            await asyncio.sleep(0.5)  # 0.5秒间隔
    except KeyboardInterrupt:
        print("\n[Client] Interrupted")
    finally:
        await asyncio.sleep(1)  # 等待最后一批回复到达
        await conn.close()
        print("[Client] Closed")


async def send_request(channel, exchange, seq: int):
    """发送请求到队列1"""
    msg_id = uuid.uuid4().hex
    ts = datetime.now().strftime('%Y%m%d%H%M%S') + '000'

    # 构建请求包
    pkt = MsgPacket(MSG_TYPE_REQUEST, "V1.0")
    pkt.set_msg_id(msg_id)
    pkt.set_timestamp(ts)
    pkt.set_func("echo")
    pkt.set_headers(2, "seq,data")
    pkt.add_row()
    pkt.set_value("seq", str(seq))
    pkt.set_value("data", f"request-{seq}")
    pkt.finalize()

    _, wire_data = pkt.encode()

    # 发送到队列1
    await exchange.publish(
        Message(body=wire_data),
        routing_key=QUEUE_REQ,
    )
    print(f"[Client] -> request[{seq}]: msg_id={msg_id}, "
          f"func=echo, seq={seq}, size={len(wire_data)}")


if __name__ == "__main__":
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        loop.run_until_complete(main())
    except KeyboardInterrupt:
        print("\n[Client] Exited")
    finally:
        loop.close()