#!/usr/bin/env python3
"""RabbitMQ RPC Demo - 单回调队列 + msg_id 关联，支持并发请求"""

import asyncio
import time
import uuid

import aio_pika

from msgpacket import MsgPacket, MSG_TYPE_REQUEST

RABBITMQ_URL = "amqp://192.168.10.2:5672/"
EXCHANGE_NAME = "msgpacket.exchange"
ROUTING_KEY_REQ = "EvTrade.Req"


def build_order_msg(symbol: str, price: str, volume: str) -> tuple:
    """构建 neworder 请求，返回 (msg_id, wire_bytes)"""
    msg_id = uuid.uuid4().hex[:16]
    pkt = MsgPacket(MSG_TYPE_REQUEST, "V1.0")
    pkt.set_msg_id(msg_id)
    pkt.set_func("neworder")
    pkt.set_timestamp(time.strftime("%Y%m%d%H%M%S") + "%03d" % (int(time.time() * 1000) % 1000))

    pkt.set_headers(4, "Symbol,Price,Volume,Timestamp")
    pkt.add_row()
    pkt.set_value("Symbol", symbol)
    pkt.set_value("Price", price)
    pkt.set_value("Volume", volume)
    pkt.set_value("Timestamp", pkt.timestamp())

    print("[req]", pkt.wire_to_string())
    ret = pkt.finalize()
    if ret != 0:
        raise RuntimeError(f"msg_finalize failed: {ret}")

    code, wire = pkt.encode()
    if code != 0:
        raise RuntimeError(f"msg_encode failed: {code}")
    return msg_id, wire


async def rpc_call(exchange, callback_queue, msg_id, wire, timeout=10):
    """发送一个 RPC 请求，等匹配的应答返回"""
    await exchange.publish(
        aio_pika.Message(body=wire, reply_to=callback_queue.name),
        routing_key=ROUTING_KEY_REQ,
    )
    print(f"[Req] msg_id={msg_id}, size={len(wire)} bytes -> [{ROUTING_KEY_REQ}]")

    async with callback_queue.iterator() as qiter:
        async for msg in qiter:
            wire_data = msg.body
            try:
                pkt = MsgPacket.decode(wire_data)
            except RuntimeError as e:
                print(f"  [Decode Error] {e}")
                await msg.ack()
                continue

            resp_msg_id = pkt.msg_id().strip()
            if resp_msg_id != msg_id:
                # 不是给我的应答，跳过
                print(f"  [Skip] msg_id={resp_msg_id} (not mine)")
                await msg.ack()
                continue

            # 匹配成功
            print(f"[Ans] msg_id={resp_msg_id} (MATCH), {len(wire_data)} bytes")
            print("[ans]", pkt.wire_to_string())
            print(f"  func:      {pkt.func().strip()}")
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
            return pkt

    return None


async def main():
    conn = await aio_pika.connect_robust(RABBITMQ_URL)
    print(f"Connected to RabbitMQ: {RABBITMQ_URL}")

    async with conn:
        channel = await conn.channel()
        await channel.set_qos(prefetch_count=1)
        exchange = await channel.declare_exchange(
            EXCHANGE_NAME, aio_pika.ExchangeType.TOPIC, durable=True,
        )

        # 整个连接只建一个回调队列，所有请求复用
        callback_queue = await channel.declare_queue(exclusive=True)
        print(f"[RPC] Callback queue: {callback_queue.name} (reused for all requests)")

        # 发送单个请求
        msg_id, wire = build_order_msg("513050.SH", "1.500", "100")
        await rpc_call(exchange, callback_queue, msg_id, wire)

        # 并发多个请求示例（每个请求用自己的 msg_id 区分）
        # tasks = []
        # for i in range(3):
        #     mid, w = build_order_msg(f"00000{i}.SH", "1.500", "100")
        #     tasks.append(rpc_call(exchange, callback_queue, mid, w))
        # await asyncio.gather(*tasks)

    print("Publisher closed.")


if __name__ == "__main__":
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(main())
    loop.close()
