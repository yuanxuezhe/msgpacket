#!/usr/bin/env python3
"""
Demo2: RPC Server - 从队列1获取请求，处理后返回应答到队列2
先启动 server，再启动 client，server 收满 5 条请求后自动退出。

用法:
    python demo_rpc_server.py
"""

import asyncio
import json
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


async def main():
    conn = await aio_pika.connect_robust(RABBITMQ_URL)
    print(f"[Server] Connected: {RABBITMQ_URL}", flush=True)

    received = 0

    async with conn:
        channel = await conn.channel()
        await channel.set_qos(prefetch_count=1)

        exchange = await channel.declare_exchange(
            EXCHANGE_NAME, ExchangeType.TOPIC, durable=True,
        )

        req_queue = await channel.declare_queue(QUEUE_REQ, durable=True)
        await req_queue.bind(exchange, routing_key=QUEUE_REQ)
        print(f"[Server] Listening on [{QUEUE_REQ}] ...", flush=True)

        # Wait for binding to take effect
        await asyncio.sleep(0.5)

        while received < EXPECTED_REQUESTS:
            try:
                message = await req_queue.get(timeout=5)
            except asyncio.TimeoutError:
                print(f"[Server] timeout, retrying...", flush=True)
                continue
            except QueueEmpty:
                print(f"[Server] QueueEmpty, retrying...", flush=True)
                await asyncio.sleep(0.5)
                continue

            async with message.process():
                wire_data = message.body

                try:
                    pkt = MsgPacket.decode(wire_data)
                except RuntimeError as e:
                    print(f"[Server] decode error: {e}", flush=True)
                    continue

                req_msg_id = pkt.msg_id().strip()
                req_func = pkt.func().strip('\x00')
                print(f"[Server] <- request: msg_id={req_msg_id}, "
                      f"func={req_func}, size={len(wire_data)}", flush=True)

                result = handle_request(pkt)
                print(f"[Server] handle -> {result}", flush=True)

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

                await channel.default_exchange.publish(
                    aio_pika.Message(body=ans_wire),
                    routing_key=QUEUE_REPLY,
                )
                print(f"[Server] -> reply: {len(ans_wire)} bytes "
                      f"to [{QUEUE_REPLY}], msg_id={req_msg_id}", flush=True)

                received += 1
                print(f"[Server] processed {received}/{EXPECTED_REQUESTS}", flush=True)

        print(f"[Server] Done, received all {received} requests.", flush=True)


if __name__ == "__main__":
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        loop.run_until_complete(main())
    except KeyboardInterrupt:
        print("\n[Server] Exited")
    finally:
        loop.close()