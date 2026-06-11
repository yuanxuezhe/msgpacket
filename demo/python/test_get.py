#!/usr/bin/env python3
"""
Minimal test: server uses get() only, no iterator/consume
"""

import asyncio
from datetime import datetime

import aio_pika
from aio_pika import ExchangeType
from aio_pika.exceptions import QueueEmpty

from msgpacket import MsgPacket, MSG_TYPE_ANSWER


RABBITMQ_URL = "amqp://192.168.10.2:5672/"
EXCHANGE_NAME = "msgpacket.exchange"
QUEUE_REQ = "EvTrade.Req"
QUEUE_REPLY = "EvTrade.Reply"
EXPECTED_REQUESTS = 5


async def main():
    conn = await aio_pika.connect_robust(RABBITMQ_URL)
    print(f"[Server] Connected", flush=True)

    async with conn:
        channel = await conn.channel()
        await channel.set_qos(prefetch_count=1)

        exchange = await channel.declare_exchange(
            EXCHANGE_NAME, ExchangeType.TOPIC, durable=True,
        )

        req_queue = await channel.declare_queue(QUEUE_REQ, durable=True)
        await req_queue.bind(exchange, routing_key=QUEUE_REQ)
        print(f"[Server] Queue declared and bound, calling get()...", flush=True)

        for i in range(EXPECTED_REQUESTS):
            print(f"[Server] calling get() attempt {i+1}...", flush=True)
            try:
                message = await req_queue.get(timeout=5)
                print(f"[Server] got message: {message}", flush=True)
            except QueueEmpty:
                print(f"[Server] QueueEmpty on attempt {i+1}", flush=True)
                i -= 1  # retry
            except asyncio.TimeoutError:
                print(f"[Server] TimeoutError on attempt {i+1}", flush=True)
                i -= 1  # retry

        print(f"[Server] Done", flush=True)
        await conn.close()


if __name__ == "__main__":
    asyncio.get_event_loop().run_until_complete(main())