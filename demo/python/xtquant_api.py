#!/usr/bin/env python3
# -*- coding: gbk -*-
"""
XtQuant API + msgpacket RPC Server

整合 QMT 交易接口和 RabbitMQ RPC，支持：
1. 从 RabbitMQ 接收交易请求（查持仓/订单/资产等），处理后返回应答
2. 将 QMT 回调事件（成交/委托/错误）推送到 RabbitMQ

用法:
    python xtquant_api.py
"""

import asyncio
import json
import signal
import sys
import threading
import time
from datetime import datetime
from queue import Empty, Queue
from typing import Optional

import aio_pika
from aio_pika import ExchangeType
from aio_pika.exceptions import QueueEmpty

from msgpacket import MsgPacket, MSG_TYPE_ANSWER, MSG_TYPE_PUSH

# ================================================================
# XtQuantTrader 相关
# ================================================================
try:
    from xtquant.xttrader import XtQuantTrader, XtQuantTraderCallback
    from xtquant.xttype import StockAccount
    from xtquant import xtconstant
    XTQUANT_AVAILABLE = True
except ImportError:
    XTQUANT_AVAILABLE = False
    print("[Warning] XtQuantTrader not available, running in RabbitMQ-only mode")

# ================================================================
# 配置
# ================================================================
RABBITMQ_URL = "amqp://192.168.10.2:5672/"
EXCHANGE_NAME = "msgpacket.exchange"
QUEUE_REQ = "EvTrade.Req"       # 队列1：接收请求（客户端→API）
QUEUE_REPLY = "EvTrade.Reply"   # 队列2：返回应答（API→客户端）
QUEUE_PUSH = "EvTrade.Push"     # 队列3：主动推送（API→客户端）

ACCOUNT_PATH = r"D:\software\trade\iQuant\userdata"
ACCOUNT_ID = "410001265100"

# 全局变量
xt_trader: Optional[XtQuantTrader] = None
xt_acc = None
event_queue: Queue = Queue()
loop: Optional[asyncio.AbstractEventLoop] = None
shutdown_event: asyncio.Event = None


# ================================================================
# 交易请求处理
# ================================================================
def handle_trade_request(pkt: MsgPacket) -> dict:
    """处理交易相关请求"""
    func = pkt.func().strip('\x00')

    if not XTQUANT_AVAILABLE or xt_trader is None:
        return {"code": "99999", "error": "交易接口未连接"}

    try:
        if func == "qry_pos":
            return handle_query_positions()
        elif func == "qry_ord":
            return handle_query_orders()
        elif func == "qry_ast":
            return handle_query_asset()
        elif func == "ord_stk":
            return handle_order_stock(pkt)
        elif func == "cxl_ord":
            return handle_cancel_order(pkt)
        else:
            return {"code": "99999", "error": f"unknown func: {func}"}
    except Exception as e:
        return {"code": "99999", "error": str(e)}


def handle_query_positions() -> dict:
    """查询持仓"""
    global xt_trader, xt_acc
    positions = xt_trader.query_stock_positions(xt_acc)
    rows = []
    for pos in positions:
        rows.append({
            "stock_code": pos.stock_code,
            "volume": pos.volume,
            "can_sell": pos.can_sell,
            "avg_cost": pos.avg_cost,
            "market_value": pos.market_value,
        })
    return {"code": "00000", "positions": rows}


def handle_query_orders() -> dict:
    """查询当日委托"""
    global xt_trader, xt_acc
    orders = xt_trader.query_stock_orders(xt_acc)
    rows = []
    for order in orders:
        rows.append({
            "order_id": order.order_id,
            "stock_code": order.stock_code,
            "price": order.price,
            "order_volume": order.order_volume,
            "traded_volume": order.traded_volume,
            "order_status": order.order_status,
        })
    return {"code": "00000", "orders": rows}


def handle_query_asset() -> dict:
    """查询账户资产"""
    global xt_trader, xt_acc
    asset = xt_trader.query_stock_asset(xt_acc)
    if asset is None:
        return {"code": "99999", "error": "查询资产失败"}
    return {
        "code": "00000",
        "asset": {
            "account_id": asset.account_id,
            "cash": asset.cash,
            "frozen_cash": asset.frozen_cash,
            "market_value": asset.market_value,
            "total_asset": asset.total_asset,
        }
    }


def handle_order_stock(pkt: MsgPacket) -> dict:
    """下单"""
    global xt_trader, xt_acc
    stock_code = pkt.get_value_str("stock_code")
    volume = int(pkt.get_value_str("volume"))
    price_type_str = pkt.get_value_str("price_type")
    price = float(pkt.get_value_str("price"))

    price_type_map = {
        "LATEST_PRICE": xtconstant.LATEST_PRICE,
        "LIMIT_PRICE": xtconstant.LIMIT_PRICE,
    }
    price_type = price_type_map.get(price_type_str, xtconstant.LATEST_PRICE)

    # 获取买卖方向
    direction_str = pkt.get_value_str("direction")
    direction = xtconstant.STOCK_BUY if direction_str == "BUY" else xtconstant.STOCK_SELL

    seq = xt_trader.order_stock_async(
        xt_acc, stock_code, direction, volume,
        price_type, price,
        "xtquant_api", f"api_{int(time.time())}"
    )
    return {"code": "00000", "seq": seq}


def handle_cancel_order(pkt: MsgPacket) -> dict:
    """撤单"""
    global xt_trader, xt_acc
    order_id = pkt.get_value_str("order_id")
    market_str = pkt.get_value_str("market")
    market = xtconstant.SZ_MARKET if market_str == "SZ" else xtconstant.SH_MARKET
    result = xt_trader.cancel_order_stock_async(xt_acc, market, order_id)
    return {"code": "00000", "result": result}


# ================================================================
# XtQuantTrader 回调 → RabbitMQ 推送
# ================================================================
class MyXtQuantTraderCallback(XtQuantTraderCallback):
    """XtQuantTrader 回调实现"""

    def on_disconnected(self):
        print(f"[{datetime.now().strftime('%H:%M:%S')}] [Callback] 连接断开，将重连", flush=True)
        global xt_trader
        xt_trader = None
        # 通知主线程重连
        event_queue.put(("disconnected", None))

    def on_stock_order(self, order):
        print(f"[{datetime.now().strftime('%H:%M:%S')}] [Callback] 委托: {order.stock_code} "
              f"{order.order_id} 状态:{order.order_status}", flush=True)
        # 转发到 RabbitMQ
        push_event("ord_cfm", {
            "order_id": order.order_id,
            "stock_code": order.stock_code,
            "order_status": order.order_status,
            "order_volume": order.order_volume,
            "traded_volume": order.traded_volume,
            "price": order.price,
            "traded_price": order.traded_price,
        })

    def on_stock_trade(self, trade):
        print(f"[{datetime.now().strftime('%H:%M:%S')}] [Callback] 成交: {trade.stock_code} "
              f"数量:{trade.traded_volume} 价格:{trade.traded_price}", flush=True)
        push_event("trd_cfm", {
            "traded_id": trade.traded_id,
            "stock_code": trade.stock_code,
            "traded_volume": trade.traded_volume,
            "traded_price": trade.traded_price,
            "account_id": trade.account_id,
        })

    def on_order_error(self, order_error):
        print(f"[{datetime.now().strftime('%H:%M:%S')}] [Callback] 报单失败: "
              f"{order_error.order_id} {order_error.error_msg}", flush=True)
        push_event("ord_err", {
            "order_id": order_error.order_id,
            "error_msg": order_error.error_msg,
        })

    def on_cancel_error(self, cancel_error):
        print(f"[{datetime.now().strftime('%H:%M:%S')}] [Callback] 撤单失败: "
              f"{cancel_error.order_id} {cancel_error.error_msg}", flush=True)
        push_event("cxl_err", {
            "order_id": cancel_error.order_id,
            "error_msg": cancel_error.error_msg,
        })

    def on_order_stock_async_response(self, response):
        print(f"[{datetime.now().strftime('%H:%M:%S')}] [Callback] 异步下单响应: "
              f"seq={response.seq} order_id={response.order_id}", flush=True)
        push_event("ord_ack", {
            "seq": response.seq,
            "order_id": response.order_id,
        })

    def on_account_status(self, status):
        print(f"[{datetime.now().strftime('%H:%M:%S')}] [Callback] 账号状态: "
              f"{status.account_id} -> {status.status}", flush=True)
        push_event("acc_sts", {
            "account_id": status.account_id,
            "status": status.status,
        })


# ================================================================
# RabbitMQ 推送
# ================================================================
def push_event(event_type: str, data: dict):
    """将事件推送到 RabbitMQ（通过 asyncio 队列）"""
    if loop is None:
        return
    asyncio.run_coroutine_threadsafe(
        _async_push_event(event_type, data),
        loop
    )


async def _async_push_event(event_type: str, data: dict):
    """异步推送事件到 RabbitMQ"""
    global loop
    if loop is None or loop.is_closed():
        return
    try:
        conn = await aio_pika.connect_robust(RABBITMQ_URL, loop=loop)
    except Exception as e:
        print(f"[Push] 连接 RabbitMQ 失败: {e}", flush=True)
        return

    async with conn:
        channel = await conn.channel()
        exchange = await channel.declare_exchange(
            EXCHANGE_NAME, ExchangeType.TOPIC, durable=True,
        )

        pkt = MsgPacket(MSG_TYPE_PUSH)
        pkt.set_func(event_type)
        pkt.set_timestamp(datetime.now().strftime('%Y%m%d%H%M%S%f')[:-3])
        pkt.set_msg_id(f"push_{int(time.time()*1000)}")
        pkt.set_headers(2, "key,value")
        pkt.add_row()
        pkt.set_value("key", event_type)
        pkt.set_value("value", json.dumps(data, ensure_ascii=False))
        pkt.finalize()

        _, wire = pkt.encode()
        await exchange.publish(
            aio_pika.Message(body=wire),
            routing_key=QUEUE_PUSH,
        )
        # print(f"[Push] -> {event_type} to [{QUEUE_PUSH}]", flush=True)


# ================================================================
# XtQuantTrader 连接管理
# ================================================================
def create_trader(session_id: int):
    """创建交易连接"""
    if not XTQUANT_AVAILABLE:
        return None
    try:
        callback = MyXtQuantTraderCallback()
        trader = XtQuantTrader(ACCOUNT_PATH, session_id, callback=callback)
        trader.start()
        result = trader.connect()
        if result == 0:
            trader.subscribe(xt_acc)
            print(f"[Trader] 连接成功, session_id={session_id}", flush=True)
            return trader
        else:
            print(f"[Trader] 连接失败, session_id={session_id}, result={result}", flush=True)
            return None
    except Exception as e:
        print(f"[Trader] 异常, session_id={session_id}: {e}", flush=True)
        return None


def try_connect() -> Optional[XtQuantTrader]:
    """尝试连接，重试多个 session_id"""
    import random
    session_ids = list(range(100, 130))
    random.shuffle(session_ids)

    for sid in session_ids:
        trader = create_trader(sid)
        if trader:
            return trader
        print(f"[Trader] session_id={sid} 失败，尝试下一个...", flush=True)
        time.sleep(0.5)

    print("[Trader] 所有 session_id 都尝试后仍失败", flush=True)
    return None


def ensure_connected():
    """确保交易接口已连接"""
    global xt_trader
    if xt_trader is None:
        xt_trader = try_connect()
    return xt_trader is not None


# ================================================================
# RabbitMQ RPC Server
# ================================================================
async def rpc_server():
    """RPC Server：从队列接收请求，处理后返回应答"""
    global shutdown_event

    print(f"[RPC] Connecting to {RABBITMQ_URL}", flush=True)
    conn = await aio_pika.connect_robust(RABBITMQ_URL)
    print(f"[RPC] Connected", flush=True)

    async with conn:
        channel = await conn.channel()
        await channel.set_qos(prefetch_count=1)

        exchange = await channel.declare_exchange(
            EXCHANGE_NAME, ExchangeType.TOPIC, durable=True,
        )

        # 声明请求队列
        req_queue = await channel.declare_queue(QUEUE_REQ, durable=True)
        await req_queue.bind(exchange, routing_key=QUEUE_REQ)
        print(f"[RPC] Listening on [{QUEUE_REQ}]", flush=True)

        # 等待绑定生效
        await asyncio.sleep(0.3)

        # 使用 iterator 消费消息
        async with req_queue.iterator() as qiter:
            async for message in qiter:
                if shutdown_event.is_set():
                    break

                async with message.process():
                    try:
                        wire_data = message.body
                        pkt = MsgPacket.decode(wire_data)
                    except Exception as e:
                        print(f"[RPC] Decode error: {e}", flush=True)
                        continue

                    req_msg_id = pkt.msg_id().strip()
                    req_func = pkt.func().strip('\x00')
                    print(f"[RPC] <- request: msg_id={req_msg_id}, func={req_func}", flush=True)

                    # 处理请求（同步调用）
                    result = handle_trade_request(pkt)
                    print(f"[RPC] handle -> {result['code']}", flush=True)

                    # 构建应答
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
                    print(f"[RPC] -> reply to [{QUEUE_REPLY}], msg_id={req_msg_id}", flush=True)


# ================================================================
# 主程序
# ================================================================
def event_loop_thread():
    """在独立线程中运行 asyncio event loop"""
    global loop, shutdown_event

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    shutdown_event = asyncio.Event()

    # 注册 signal handler
    def signal_handler(sig, frame):
        print("\n[Main] Shutdown signal received", flush=True)
        shutdown_event.set()
        # 取消所有任务
        for task in asyncio.all_tasks(loop):
            task.cancel()

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    try:
        loop.run_until_complete(rpc_server())
    except asyncio.CancelledError:
        print("[Main] RPC server cancelled", flush=True)
    finally:
        print("[Main] Event loop exiting", flush=True)
        loop.close()


def process_events():
    """处理事件队列（主线程调用，检查连接状态等）"""
    global xt_trader
    while True:
        try:
            event_type, data = event_queue.get(timeout=1)
            if event_type == "disconnected":
                print("[Main] 检测到断开，尝试重连...", flush=True)
                time.sleep(2)
                if ensure_connected():
                    print("[Main] 重连成功", flush=True)
        except Empty:
            pass


def main():
    print("=" * 60)
    print("XtQuant API + msgpacket RPC Server")
    print("=" * 60)

    # 初始化账户
    global xt_acc
    if XTQUANT_AVAILABLE:
        xt_acc = StockAccount(ACCOUNT_ID)
        print(f"[Main] 账户: {ACCOUNT_ID}", flush=True)

        # 初始连接
        if not ensure_connected():
            print("[Main] 初始连接失败，RPC server 仍会启动", flush=True)
    else:
        print("[Main] XtQuant 不可用，仅运行 RPC server", flush=True)

    # 启动 asyncio event loop 在独立线程
    thread = threading.Thread(target=event_loop_thread, daemon=True)
    thread.start()
    print("[Main] RPC server 线程已启动", flush=True)

    # 主线程处理事件（重连检测）
    try:
        process_events()
    except KeyboardInterrupt:
        print("\n[Main] KeyboardInterrupt", flush=True)
        if shutdown_event:
            shutdown_event.set()


if __name__ == "__main__":
    main()
