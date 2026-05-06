#!/usr/bin/env python3
"""RabbitMQ Request-Reply Demo - 发请求到 EvTrade.Req，等 EvTrade.Ans 应答"""

import asyncio
import time
import uuid

import aio_pika

from msgpacket import MsgPacket, MSG_TYPE_REQUEST

RABBITMQ_URL = "amqp://192.168.10.2:5672/"
EXCHANGE_NAME = "msgpacket.exchange"
ROUTING_KEY_REQ = "EvTrade.Req"
QUEUE_ANS = "EvTrade.Ans"


def build_order_msg(symbol: str, price: str, volume: str) -> tuple:
    """构建 new_order 请求，返回 (msg_id, wire_bytes)"""
    msg_id = uuid.uuid4().hex[:16]
    pkt = MsgPacket(MSG_TYPE_REQUEST, "V1.0")
    pkt.set_msg_id(msg_id)
    pkt.set_func("new_order")
    pkt.set_timestamp(time.strftime("%Y%m%d%H%M%S") + "%03d" % (int(time.time() * 1000) % 1000))

    pkt.set_headers(4, "Symbol,Price,Volume,Timestamp")
    pkt.add_row()
    pkt.set_value("Symbol", symbol)
    pkt.set_value("Price", price)
    pkt.set_value("Volume", volume)
    pkt.set_value("Timestamp", pkt.timestamp())

    ret = pkt.finalize()
    if ret != 0:
        raise RuntimeError(f"msg_finalize failed: {ret}")

    code, wire = pkt.encode()
    if code != 0:
        raise RuntimeError(f"msg_encode failed: {code}")
    return msg_id, wire


async def main():
    conn = await aio_pika.connect_robust(RABBITMQ_URL)
    print(f"Connected to RabbitMQ: {RABBITMQ_URL}")

    async with conn:
        channel = await conn.channel()
        await channel.set_qos(prefetch_count=1)
        exchange = await channel.declare_exchange(
            EXCHANGE_NAME, aio_pika.ExchangeType.TOPIC, durable=True,
        )

        # 声明 EvTrade.Ans 队列用于收应答
        ans_queue = await channel.declare_queue(QUEUE_ANS, durable=True)
        await ans_queue.bind(exchange, routing_key=QUEUE_ANS)

        # 构建并发送请求
        msg_id, wire = build_order_msg("513050.SH", "1.500", "100")
        await exchange.publish(
            aio_pika.Message(body=wire),
            routing_key=ROUTING_KEY_REQ,
        )
        print(f"[Req] Published to [{ROUTING_KEY_REQ}]: msg_id={msg_id}, size={len(wire)} bytes")

        # 阻塞等待应答
        print(f"[Ans] Waiting for response on [{QUEUE_ANS}] ...")
        async with ans_queue.iterator() as qiter:
            async for msg in qiter:
                wire_data = msg.body
                print(f"[Ans] Received: {len(wire_data)} bytes")

                try:
                    pkt = MsgPacket.decode(wire_data)
                except RuntimeError as e:
                    print(f"  [Decode Error] {e}")
                    await msg.ack()
                    continue

                resp_msg_id = pkt.msg_id().strip()
                resp_func = pkt.func().strip()
                matched = "MATCH" if resp_msg_id == msg_id else "MISMATCH"
                print(f"  msg_id:    {resp_msg_id} ({matched})")
                print(f"  func:      {resp_func}")
                print(f"  msg_type:  {pkt.msg_type_name()} (0x{pkt.msg_type():02X})")
                print(f"  timestamp: {pkt.timestamp()}")
                print(f"  headers:   {pkt.get_headers()}")

                pkt.reset_cursor()
                while pkt.fetch_next():
                    row = pkt.current_row()
                    print(f"    row {row}:", end="")
                    for col in range(pkt.header_count()):
                        val = pkt.get_field(row, col)
                        print(f" {val}", end="")
                    print()

                await msg.ack()
                break  # 收到一条应答就退出

    print("Publisher closed.")


if __name__ == "__main__":
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(main())
    loop.close()
