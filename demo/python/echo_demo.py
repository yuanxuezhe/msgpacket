#!/usr/bin/env python3
"""RabbitMQ Echo Demo - 收什么就反射什么，通过 reply_to 回传给请求端"""

import asyncio

import aio_pika

from msgpacket import MsgPacket

RABBITMQ_URL = "amqp://192.168.10.2:5672/"
EXCHANGE_NAME = "msgpacket.exchange"
QUEUE_REQ = "EvTrade.Req"


async def main():
    conn = await aio_pika.connect_robust(RABBITMQ_URL)
    print(f"Connected: {RABBITMQ_URL}")

    async with conn:
        channel = await conn.channel()
        await channel.set_qos(prefetch_count=1)
        exchange = await channel.declare_exchange(
            EXCHANGE_NAME, aio_pika.ExchangeType.TOPIC, durable=True,
        )

        req_queue = await channel.declare_queue(QUEUE_REQ, durable=True)
        await req_queue.bind(exchange, routing_key=QUEUE_REQ)
        print(f"[Echo] Listening on [{QUEUE_REQ}] ...")

        async with req_queue.iterator() as qiter:
            async for msg in qiter:
                wire_data = msg.body
                reply_to = msg.reply_to

                try:
                    pkt = MsgPacket.decode(wire_data)
                except RuntimeError as e:
                    print(f"  [Decode Error] {e}")
                    await msg.ack()
                    continue

                req_msg_id = pkt.msg_id()
                req_func = pkt.func()
                print(f"  [Echo] msg_id={req_msg_id}, func={req_func}, size={len(wire_data)}")
                if reply_to:
                    print(f"  [req] {pkt.wire_to_string()}")
                else:
                    print(f"  [req] {pkt.wire_to_string()}  (no reply_to, skipping)")

                # 通过 reply_to 回传给请求端的回调队列
                # 使用 default_exchange 直接投递到队列，不走 topic exchange
                if reply_to:
                    await channel.default_exchange.publish(
                        aio_pika.Message(body=wire_data),
                        routing_key=reply_to,
                    )
                    print(f"  [ans] reflected, {len(wire_data)} bytes -> [{reply_to}]")
                else:
                    print(f"  [ans] no reply_to, cannot respond")

                await msg.ack()


if __name__ == "__main__":
    try:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        loop.run_until_complete(main())
    except KeyboardInterrupt:
        print("\nExiting...")
    finally:
        loop.close()
