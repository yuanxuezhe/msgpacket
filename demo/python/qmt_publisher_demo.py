#!/usr/bin/env python3
# coding: gbk
"""QMT + RabbitMQ 双向网关 Demo

入站: 消费 EvTrade.Req → 解码 MsgPacket → QMT 执行交易
出站: QMT 回调 → MsgPacket 编码 → 发布到 EvTrade.Ans

依赖:
    pip install aio-pika
    xtquant (QMT 客户端)

架构:
    主线程 (asyncio):  RabbitMQ 连接 + 消费 EvTrade.Req + 永久阻塞
    后台线程 (daemon): xt_trader.run_forever()
    QMT 回调:          asyncio.run_coroutine_threadsafe(publish, loop)
"""

import asyncio
import time
import threading

import aio_pika

from msgpacket import MsgPacket, MSG_TYPE_PUSH, MSG_TYPE_ANSWER

# ================================================================
# 配置
# ================================================================
RABBITMQ_URL = "amqp://192.168.10.2:5672/"
EXCHANGE_NAME = "msgpacket.exchange"
ROUTING_KEY_REQ = "EvTrade.Req"
QUEUE_REQ = "EvTrade.Req"
ROUTING_KEY_ANS = "EvTrade.Ans"

XT_PATH = r'D:\software\trade\guosen\iQuant\userdata'
XT_ACCOUNT_ID = '410001265100'

# ================================================================
# RabbitMQ 网关（发布 + 消费）
# ================================================================
class RabbitMQGateway:
    """封装 RabbitMQ 双向通信：消费 EvTrade.Req，发布到 EvTrade.Ans"""

    def __init__(self):
        self.conn = None
        self.channel = None
        self.exchange = None

    async def connect(self):
        self.conn = await aio_pika.connect_robust(RABBITMQ_URL)
        self.channel = await self.conn.channel()
        await self.channel.set_qos(prefetch_count=1)
        self.exchange = await self.channel.declare_exchange(
            EXCHANGE_NAME, aio_pika.ExchangeType.TOPIC, durable=True,
        )
        # 声明两个队列
        self.req_queue = await self.channel.declare_queue(QUEUE_REQ, durable=True)
        await self.req_queue.bind(self.exchange, routing_key=ROUTING_KEY_REQ)
        self.ans_queue = await self.channel.declare_queue(ROUTING_KEY_ANS, durable=True)
        await self.ans_queue.bind(self.exchange, routing_key=ROUTING_KEY_ANS)
        print(f"[RabbitMQ] Connected, bound [{QUEUE_REQ}] and [{ROUTING_KEY_ANS}]")

    async def close(self):
        if self.conn:
            await self.conn.close()
            print("[RabbitMQ] Disconnected")

    async def publish_answer(self, msg_id: str, func: str, headers: list, values: list):
        """构建 MsgPacket 并发布到 EvTrade.Ans，回传请求的 msg_id 和 func"""
        pkt = MsgPacket(MSG_TYPE_ANSWER, "V1.0")
        pkt.set_msg_id(msg_id)
        pkt.set_func(func)
        pkt.set_timestamp(time.strftime("%Y%m%d%H%M%S") + "%03d" % (int(time.time() * 1000) % 1000))

        ncols = len(headers)
        header_str = ",".join(headers)
        pkt.set_headers(ncols, header_str)
        pkt.add_row()
        for h, v in zip(headers, values):
            pkt.set_value(h, str(v))

        ret = pkt.finalize()
        if ret != 0:
            print(f"  [MsgPacket] finalize failed: {ret}")
            return

        code, wire = pkt.encode()
        if code != 0:
            print(f"  [MsgPacket] encode failed: {code}")
            return

        await self.exchange.publish(
            aio_pika.Message(body=wire),
            routing_key=ROUTING_KEY_ANS,
        )
        print(f"  [Published->Ans] func={func}, size={len(wire)} bytes")

    async def consume_requests(self, xt_trader_ref: list, acc):
        """消费 EvTrade.Req 队列，收到请求后执行 QMT 交易"""
        async with self.req_queue.iterator() as qiter:
            async for msg in qiter:
                wire_data = msg.body
                print(f"[Req] Received: {len(wire_data)} bytes")

                try:
                    pkt = MsgPacket.decode(wire_data)
                except RuntimeError as e:
                    print(f"  [Req Decode Error] {e}")
                    await self.publish_answer("", "decode_error", ["Error"], [str(e)])
                    await msg.ack()
                    continue

                func = pkt.func().strip()
                req_msg_id = pkt.msg_id().strip()
                print(f"  [Req] msg_id={req_msg_id}, func={func}")

                if func == "new_order":
                    await self._handle_new_order(req_msg_id, func, pkt, xt_trader_ref, acc)
                else:
                    await self.publish_answer(req_msg_id, func,
                        ["Error"], [f"unknown func: {func}"])

                await msg.ack()

    async def _handle_new_order(self, msg_id: str, func: str, pkt: MsgPacket,
                                 xt_trader_ref: list, acc):
        """处理 new_order 请求：提取参数并下单，应答复用请求的 msg_id 和 func"""
        symbol = pkt.get_value_str("Symbol")
        price = pkt.get_value_str("Price")
        volume = pkt.get_value_str("Volume")

        if not symbol:
            await self.publish_answer(msg_id, func, ["Error"], ["missing Symbol"])
            return

        trader = xt_trader_ref[0]
        if trader is None:
            await self.publish_answer(msg_id, func, ["Error"], ["trader not connected"])
            return

        try:
            vol = int(volume) if volume else 100
        except ValueError:
            vol = 100

        try:
            from xtquant import xtconstant
            trader.order_stock_async(
                acc, symbol, xtconstant.STOCK_BUY, vol,
                xtconstant.LATEST_PRICE, 0
            )
            print(f"  [QMT] 下单: {symbol} BUY {vol}")
            await self.publish_answer(msg_id, func,
                ["Symbol", "Volume", "Status"],
                [symbol, str(vol), "sent"])
        except Exception as e:
            print(f"  [QMT] 下单失败: {e}")
            await self.publish_answer(msg_id, func,
                ["Symbol", "Error"],
                [symbol, str(e)])


# ================================================================
# QMT 回调 → 发布到 EvTrade.Ans
# ================================================================
class QmtCallback:
    """XtQuantTrader 回调，将事件通过 MsgPacket 发布到 EvTrade.Ans"""

    def __init__(self, gateway: RabbitMQGateway, loop: asyncio.AbstractEventLoop,
                 xt_trader_ref: list):
        self.gw = gateway
        self.loop = loop
        self.xt_trader_ref = xt_trader_ref

    def _publish(self, func: str, headers: list, values: list):
        """发布 QMT 事件（非请求应答，msg_id 为空）"""
        try:
            asyncio.run_coroutine_threadsafe(
                self.gw.publish_answer("", func, headers, values), self.loop
            )
        except Exception as e:
            print(f"  [Publish Error] {func}: {e}")

    def on_disconnected(self):
        print("[QMT] connection lost, 交易接口断开，即将重连")
        self.xt_trader_ref[0] = None
        self._publish("disconnected", ["Reason"], ["connection_lost"])

    def on_stock_order(self, order):
        print(f"[QMT] 委托回报: {order.stock_code} 订单:{order.order_id} 状态:{order.order_status}")
        self._publish("stock_order",
            ["StockCode", "AccountID", "OrderID", "OrderSysID",
             "OrderStatus", "OrderVolume", "TradedVolume"],
            [order.stock_code, order.account_id, order.order_id, order.order_sysid,
             order.order_status, order.order_volume, order.traded_volume])

    def on_stock_trade(self, trade):
        print(f"[QMT] 成交回报: {trade.stock_code} 订单:{trade.order_id} 成交:{trade.traded_id}")
        self._publish("stock_trade",
            ["StockCode", "AccountID", "OrderID", "OrderSysID",
             "TradedID", "TradedVolume", "Direction"],
            [trade.stock_code, trade.account_id, trade.order_id, trade.order_sysid,
             trade.traded_id, trade.traded_volume, trade.direction])

    def on_order_error(self, order_error):
        print(f"[QMT] 报单失败: 订单:{order_error.order_id} {order_error.error_msg}")
        self._publish("order_error",
            ["OrderID", "ErrorMsg", "OrderRemark"],
            [order_error.order_id, order_error.error_msg, order_error.order_remark])

    def on_cancel_error(self, cancel_error):
        print(f"[QMT] 撤单失败: 订单:{cancel_error.order_id} {cancel_error.error_msg}")
        self._publish("cancel_error",
            ["OrderID", "ErrorMsg", "Market"],
            [cancel_error.order_id, cancel_error.error_msg, cancel_error.market])

    def on_order_stock_async_response(self, response):
        print(f"[QMT] 异步下单响应: seq={response.seq}, order_id={response.order_id}")
        self._publish("async_response",
            ["Seq", "OrderID"],
            [str(response.seq), str(response.order_id)])

    def on_account_status(self, status):
        print(f"[QMT] 账号状态变化: {status.account_id} status={status.status}")
        self._publish("account_status",
            ["AccountID", "Status"],
            [status.account_id, status.status])


# ================================================================
# QMT 连接管理
# ================================================================
def _create_xt_trader(path, session_id, callback):
    try:
        from xtquant.xttrader import XtQuantTrader
    except ImportError:
        print("[QMT] xtquant not installed, running in mock mode")
        return None

    trader = XtQuantTrader(path, session_id, callback=callback)
    trader.start()
    result = trader.connect()
    return trader if result == 0 else None


def try_connect_xt(path, callback):
    import random
    session_ids = list(range(100, 120))
    random.shuffle(session_ids)

    for sid in session_ids:
        trader = _create_xt_trader(path, sid, callback)
        if trader:
            print(f"[QMT] 连接成功, session_id={sid}")
            return trader
        print(f"[QMT] 连接失败, session_id={sid}，继续尝试...")

    print("[QMT] 所有 session_id 尝试后仍失败")
    return None


# ================================================================
# 主入口
# ================================================================
async def main():
    gateway = RabbitMQGateway()
    await gateway.connect()

    xt_trader_ref = [None]
    loop = asyncio.get_event_loop()
    callback = QmtCallback(gateway, loop, xt_trader_ref)

    trader = try_connect_xt(XT_PATH, callback)
    if trader is None:
        print("[QMT] 交易接口连接失败，demo 继续运行（RabbitMQ 保活）")

    xt_trader_ref[0] = trader

    qmt_thread = None
    consume_task = None

    if trader:
        def _run_qmt():
            try:
                from xtquant.xttype import StockAccount
                acc = StockAccount(XT_ACCOUNT_ID)
                trader.subscribe(acc)
            except Exception as e:
                print(f"[QMT] subscribe error: {e}")

            trader.run_forever()

        from xtquant.xttype import StockAccount
        acc = StockAccount(XT_ACCOUNT_ID)

        qmt_thread = threading.Thread(target=_run_qmt, daemon=True)
        qmt_thread.start()
        print("[QMT] 后台线程已启动")

        # 启动消费 EvTrade.Req 的协程
        consume_task = asyncio.ensure_future(
            gateway.consume_requests(xt_trader_ref, acc)
        )
        print("[Gateway] 开始消费 EvTrade.Req ...")

    print("[Main] 策略运行中，Ctrl+C 退出...")
    try:
        await asyncio.Event().wait()
    except asyncio.CancelledError:
        pass
    finally:
        print("[Main] Shutting down...")
        # 1. 停止 QMT run_forever()
        if trader:
            try:
                trader.stop()
                print("[QMT] trader.stop() called")
            except Exception as e:
                print(f"[QMT] stop error: {e}")
        # 2. 取消消费协程
        if consume_task:
            consume_task.cancel()
            try:
                await consume_task
            except asyncio.CancelledError:
                pass
        # 3. 等待 QMT 线程退出
        if qmt_thread and qmt_thread.is_alive():
            qmt_thread.join(timeout=5)
            if qmt_thread.is_alive():
                print("[QMT] 线程未能在 5s 内退出（daemon 模式，进程结束时会强制回收）")
            else:
                print("[QMT] 线程已退出")
        # 4. 关闭 RabbitMQ
        await gateway.close()
        print("[Main] 已退出")


if __name__ == "__main__":
    try:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        loop.run_until_complete(main())
    except KeyboardInterrupt:
        print("\n[Main] Ctrl+C, exiting...")
    finally:
        loop.close()
