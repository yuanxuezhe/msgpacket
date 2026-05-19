#!/usr/bin/env python3
"""
Heartbeat Demo - 心跳保活机制演示

发送方定期发送 HEARTBEAT 类型的 MsgPacket，接收方验证后回复 ACK。
演示 MSG_TYPE_HEARTBEAT 的构建、发送、接收和处理。

用法:
    # 启动接收方（终端 1）
    python demo_heartbeat.py --mode receiver

    # 启动发送方（终端 2）
    python demo_heartbeat.py --mode sender

依赖:
    - RabbitMQ 服务 (amqp://192.168.10.2:5672/)
    - msgpacket Python 库
"""

import asyncio
import time
from datetime import datetime

import aio_pika
from aio_pika import ExchangeType, Message

from msgpacket import MsgPacket, MSG_TYPE_HEARTBEAT, MSG_TYPE_ANSWER


RABBITMQ_URL = "amqp://192.168.10.2:5672/"
EXCHANGE_NAME = "msgpacket.exchange"
QUEUE_HEARTBEAT = "EvTrade.Heartbeat"


async def sender_mode():
    """发送方：定期发送心跳包"""
    conn = await aio_pika.connect_robust(RABBITMQ_URL)
    print(f"[Sender] Connected to RabbitMQ")

    async with conn:
        channel = await conn.channel()
        exchange = await channel.declare_exchange(
            EXCHANGE_NAME, ExchangeType.TOPIC, durable=True,
        )

        print("[Sender] Starting heartbeat sender (Ctrl+C to stop)...")

        seq = 0
        while True:
            seq += 1
            ts = datetime.now().strftime('%Y%m%d%H%M%S') + '%03d' % (time.time() % 1000 // 1)

            # 创建心跳包
            pkt = MsgPacket(MSG_TYPE_HEARTBEAT, "V1.0")
            pkt.set_timestamp(ts)
            pkt.set_func("heartbeat")
            pkt.set_headers(2, "seq,timestamp")
            pkt.add_row()
            pkt.set_value("seq", str(seq))
            pkt.set_value("timestamp", ts)
            pkt.finalize()

            _, wire_data = pkt.encode()

            await exchange.publish(
                Message(body=wire_data),
                routing_key=QUEUE_HEARTBEAT,
            )

            print(f"[Sender] sent heartbeat seq={seq}, ts={ts}, size={len(wire_data)}")

            # 每 5 秒发送一次
            await asyncio.sleep(5)


async def receiver_mode():
    """接收方：监听心跳包，回复 ACK"""
    conn = await aio_pika.connect_robust(RABBITMQ_URL)
    print(f"[Receiver] Connected to RabbitMQ")

    async with conn:
        channel = await conn.channel()
        await channel.set_qos(prefetch_count=10)
        exchange = await channel.declare_exchange(
            EXCHANGE_NAME, ExchangeType.TOPIC, durable=True,
        )

        queue = await channel.declare_queue(QUEUE_HEARTBEAT, durable=True)
        await queue.bind(exchange, routing_key=QUEUE_HEARTBEAT)
        print(f"[Receiver] Listening on [{QUEUE_HEARTBEAT}]...")

        last_seq = 0
        last_time = time.time()
        miss_count = 0

        async with queue.iterator() as qiter:
            async for msg in qiter:
                try:
                    pkt = MsgPacket.decode(msg.body)
                except RuntimeError as e:
                    print(f"[Receiver] decode error: {e}")
                    await msg.ack()
                    continue

                func = pkt.func().strip('\x00')
                if func != "heartbeat" or pkt.msg_type() != MSG_TYPE_HEARTBEAT:
                    print(f"[Receiver] non-heartbeat packet ignored")
                    await msg.ack()
                    continue

                # 提取心跳信息
                seq = int(pkt.get_value_str("seq") or "-1")
                ts = pkt.get_value_str("timestamp") or ""

                now = time.time()
                interval = now - last_time
                last_time = now

                # 检测丢包
                expected_seq = last_seq + 1
                if seq != expected_seq and last_seq != 0:
                    miss_count += seq - expected_seq
                    print(f"[Receiver] *** MISSED {seq - expected_seq} heartbeat(s) ***")

                last_seq = seq

                print(f"[Receiver] heartbeat recv: seq={seq}, ts={ts}, "
                      f"interval={interval:.3f}s, missed={miss_count}")

                # 发送 ACK 应答（可选）
                # ACK 包通常不需要回复，这里仅演示如何构建

                await msg.ack()


async def main():
    import argparse
    parser = argparse.ArgumentParser(description='MsgPacket Heartbeat Demo')
    parser.add_argument('--mode', choices=['sender', 'receiver'], required=True,
                        help='sender or receiver mode')
    args = parser.parse_args()

    if args.mode == 'sender':
        await sender_mode()
    else:
        await receiver_mode()


if __name__ == "__main__":
    try:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        loop.run_until_complete(main())
    except KeyboardInterrupt:
        print("\n[Exit] Interrupted")
    finally:
        loop.close()