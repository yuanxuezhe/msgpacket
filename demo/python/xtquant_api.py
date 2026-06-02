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
import random
import signal
import threading
import time
from datetime import datetime
from queue import Empty, Queue
from typing import Dict, List, Optional, Tuple

import aio_pika
from aio_pika import ExchangeType

from msgpacket import MsgPacket, MSG_TYPE_ANSWER, MSG_TYPE_PUSH
from xtquant.xttrader import XtQuantTrader, XtQuantTraderCallback
from xtquant.xttype import StockAccount
from xtquant import xtconstant

# ================================================================
# 配置
# ================================================================
RABBITMQ_URL = "amqp://192.168.10.2:5672/"
EXCHANGE_NAME = "msgpacket.exchange"
QUEUE_REQ = "EvTrade.Req"
QUEUE_REPLY = "EvTrade.Reply"
QUEUE_PUSH = "EvTrade.Push"

ACCOUNT_PATH = r"D:\software\trade\iQuant\userdata"
ACCOUNT_ID = "410001265100"

# 全局变量
xt_trader: Optional[XtQuantTrader] = None
xt_acc: Optional[StockAccount] = None
event_queue: Queue = Queue()
loop: Optional[asyncio.AbstractEventLoop] = None
shutdown_event: Optional[asyncio.Event] = None

# RabbitMQ 长连接
_mq_conn: Optional[aio_pika.RobustConnection] = None
_mq_channel: Optional[aio_pika.Channel] = None
_mq_exchange: Optional[aio_pika.Exchange] = None

# ================================================================
# Handler 返回格式: (code: str, msg: str, data: Any)
# code != 0: RS1={code,msg}      (单结果集)
# code == 0: RS1={code,msg}+RS2   (双结果集，RS2是数据表)
# ================================================================
_HANDLERS: Dict[str, callable] = {}


def _reg(func: str, handler):
    _HANDLERS[func] = handler


def handle_trade_request(pkt: MsgPacket) -> Tuple[str, str, List[Dict]]:
    func = pkt.func().strip('\x00')
    if xt_trader is None:
        return "99999", "交易接口未连接", None
    handler = _HANDLERS.get(func)
    if handler is None:
        return "99999", f"unknown func: {func}", None
    try:
        return handler(pkt)
    except Exception as e:
        return "99999", str(e), None


# ------------------------------------------------------------
def _h_qry_pos(_pkt) -> Tuple[str, str, List[Dict]]:
    positions = xt_trader.query_stock_positions(xt_acc)
    return "00000", "ok", [{
        "stock_code": pos.stock_code,
        "volume": pos.volume,
        "avl_amt": pos.can_use_volume,
        "avg_price": pos.open_price,
        "market_value": pos.market_value,
    } for pos in positions]


def _h_qry_ord(_pkt) -> Tuple[str, str, List[Dict]]:
    orders = xt_trader.query_stock_orders(xt_acc)
    return "00000", "ok", [{
        "order_id": order.order_sysid,
        "stock_code": order.stock_code,
        "price": order.price,
        "order_volume": order.order_volume,
        "traded_volume": order.traded_volume,
        "traded_price": order.traded_price,
        "order_status": order.order_status,
        "status_msg": order.status_msg,
        "strategy_name": order.strategy_name,
        "order_remark": order.order_remark,
        "order_time": order.order_time,
    } for order in orders]


def _h_qry_ast(_pkt) -> Tuple[str, str, List[Dict]]:
    asset = xt_trader.query_stock_asset(xt_acc)
    if asset is None:
        return "99999", "查询资产失败", None
    return "00000", "ok", [{
        "account_id": asset.account_id,
        "cash": asset.cash,
        "frozen_cash": asset.frozen_cash,
        "market_value": asset.market_value,
        "total_asset": asset.total_asset,
    }]

def _h_qry_mch(_pkt) -> Tuple[str, str, List[Dict]]:
    trades = xt_trader.query_stock_trades(xt_acc)
    return "00000", "ok", [{
        "order_id": trade.order_sysid,
        "traded_id": trade.traded_id,
        "stock_code": trade.stock_code,
        "traded_volume": trade.traded_volume,
        "traded_price": trade.traded_price,
        "traded_amount": trade.traded_amount,
        "strategy_name": trade.strategy_name,
        "order_remark": trade.order_remark,
        "traded_time": trade.traded_time,
    } for trade in trades]

def _h_ord_stk(pkt) -> Tuple[str, str, List[Dict]]:
    stock_code = pkt.get_value_str("stock_code")
    volume = int(pkt.get_value_str("volume"))
    price_type_str = pkt.get_value_str("price_type")
    price = float(pkt.get_value_str("price"))
    direction_str = pkt.get_value_str("direction")
    remark = pkt.get_value_str("remark")
    
    price_type_map = {
        "1": xtconstant.LATEST_PRICE,
        "0": xtconstant.FIX_PRICE,
    }
    price_type = price_type_map.get(price_type_str, xtconstant.LATEST_PRICE)
    direction = xtconstant.STOCK_BUY if direction_str == "BUY" else xtconstant.STOCK_SELL

    seq = xt_trader.order_stock_async(
        xt_acc, stock_code, direction, volume,
        price_type, price,
        "xtquant_api", remark,
    )
    return "00000", "ok", [{"seq": seq}]


def _h_cxl_ord(pkt) -> Tuple[str, str, List[Dict]]:
    order_id = pkt.get_value_str("order_id")
    market_str = pkt.get_value_str("market")
    market = xtconstant.SZ_MARKET if market_str == "SZ" else xtconstant.SH_MARKET
    result = xt_trader.cancel_order_stock_async(xt_acc, market, order_id)
    return "00000", "ok", [{"result": result}]


_reg("qry_ast", _h_qry_ast)
_reg("qry_pos", _h_qry_pos)
_reg("qry_ord", _h_qry_ord)
_reg("qry_mch", _h_qry_mch)

_reg("ord_stk", _h_ord_stk)
_reg("cxl_ord", _h_cxl_ord)


# ================================================================
# 应答组包
# ================================================================
def build_answer(pkt: MsgPacket, req_msg_id: str,
                 code: str, msg: str, data: List[Dict]) -> MsgPacket:
    """按 msgpacket 格式组应答包
    code != 0: RS1={code,msg}
    code == 0: RS1={code,msg} + RS2=data表
    """
    ts = datetime.now().strftime('%Y%m%d%H%M%S') + '000'
    ans = MsgPacket(MSG_TYPE_ANSWER, pkt.version())
    ans.set_msg_id(req_msg_id)
    ans.set_timestamp(ts)
    ans.set_func(pkt.func().strip('\x00'))

    # RS1: code + msg
    ans.set_headers(2, "code,msg")
    ans.add_row()
    ans.set_value("code", code)
    ans.set_value("msg", msg)

    # RS2: 数据表 (code==0 且有数据时才有)
    if code == "00000" and data:
        ans.add_result_set()
        cols = list(data[0].keys())
        ans.set_headers(len(cols), ",".join(cols))
        for row in data:
            ans.add_row()
            for col in cols:
                ans.set_value(col, str(row.get(col, "")))

    ans.finalize()
    return ans

# ================================================================
# XtQuantTrader 回调 → RabbitMQ 推送
# ================================================================
class MyXtQuantTraderCallback(XtQuantTraderCallback):

    def on_disconnected(self):
        print(f"[{datetime.now().strftime('%H:%M:%S')}] [Cb] 连接断开，将重连", flush=True)
        global xt_trader
        xt_trader = None
        event_queue.put(("disconnected", None))

    def on_stock_order(self, order):
        #print(f"[{datetime.now().strftime('%H:%M:%S')}] [Cb] 委托: {order.stock_code} "
        #f"{order.order_id} 状态:{order.order_status} 策略名称:{order.strategy_name}", flush=True)
        push_event("ord_cfm", [{
            "order_id": order.order_sysid,
            "stock_code": order.stock_code,
            "order_status": order.order_status,
            "order_volume": order.order_volume,
            "traded_volume": order.traded_volume,
            "price": order.price,
            "traded_price": order.traded_price,
            "strategy_name": order.strategy_name,
            "remark": order.order_remark,
            "order_time": order.order_time,
        }])

    def on_stock_trade(self, trade):
        #print(f"[{datetime.now().strftime('%H:%M:%S')}] [Cb] 成交: {trade.stock_code} "
        #      f"数量:{trade.traded_volume} 价格:{trade.traded_price}", flush=True)
        push_event("trd_cfm", [{
            "traded_id": trade.traded_id,
            "stock_code": trade.stock_code,
            "traded_volume": trade.traded_volume,
            "traded_price": trade.traded_price,
            "account_id": trade.account_id,
            "strategy_name": trade.strategy_name,
            "remark": trade.order_remark,
        }])

    def on_order_error(self, order_error):
        #print(f"[{datetime.now().strftime('%H:%M:%S')}] [Cb] 报单失败: "
        #      f"{order_error.order_id} {order_error.error_msg}", flush=True)
        push_event("ord_err", [{
            "order_id": order_error.order_id,
            "error_msg": order_error.error_msg,
        }])

    def on_cancel_error(self, cancel_error):
        #print(f"[{datetime.now().strftime('%H:%M:%S')}] [Cb] 撤单失败: "
        #      f"{cancel_error.order_id} {cancel_error.error_msg}", flush=True)
        push_event("cxl_err", [{
            "order_id": cancel_error.order_id,
            "error_msg": cancel_error.error_msg,
        }])

    def on_order_stock_async_response(self, response):
        #print(f"[{datetime.now().strftime('%H:%M:%S')}] [Cb] 异步下单响应: "
        #      f"seq={response.seq} order_id={response.order_id}", flush=True)
        push_event("ord_ack", [{
            "seq": response.seq,
            "order_id": response.order_sysid,
        }])

    def on_account_status(self, status):
        #print(f"[{datetime.now().strftime('%H:%M:%S')}] [Cb] 账号状态: "
        #      f"{status.account_id} -> {status.status}", flush=True)
        push_event("acc_sts", [{
            "account_id": status.account_id,
            "status": status.status,
        }])


# ================================================================
# RabbitMQ 推送
# ================================================================
def push_event(func: str, data: dict):
    if loop is None or loop.is_closed():
        return
    asyncio.run_coroutine_threadsafe(_mq_publish(func, data), loop)


async def _mq_publish(func: str, data: List[Dict]):
    """推送消息: func=功能号, data=RS1数据表"""
    global _mq_exchange
    if _mq_exchange is None:
        return
    try:
        pkt = MsgPacket(MSG_TYPE_PUSH)
        pkt.set_func(func)
        pkt.set_timestamp(datetime.now().strftime('%Y%m%d%H%M%S%f')[:-3])

        if data:
            cols = list(data[0].keys())
            pkt.set_headers(len(cols), ",".join(cols))
            for row in data:
                pkt.add_row()
                for col in cols:
                    pkt.set_value(col, str(row.get(col, "")))
        else:
            pkt.set_headers(0, "")

        pkt.finalize()
        print(f"PUSH:{pkt.wire_to_string()}")
        _, wire = pkt.encode()
        await _mq_exchange.publish(aio_pika.Message(body=wire), routing_key=QUEUE_PUSH)
    except Exception as e:
        print(f"[Push] 失败 {func}: {e}", flush=True)


# ================================================================
# XtQuantTrader 连接管理
# ================================================================
def create_trader(session_id: int) -> Optional[XtQuantTrader]:
    try:
        callback = MyXtQuantTraderCallback()
        trader = XtQuantTrader(ACCOUNT_PATH, session_id, callback=callback)
        trader.start()
        result = trader.connect()
        if result == 0:
            trader.subscribe(xt_acc)
            print(f"[Trader] 连接成功, session_id={session_id}", flush=True)
            return trader
        print(f"[Trader] 连接失败, session_id={session_id}, result={result}", flush=True)
        return None
    except Exception as e:
        print(f"[Trader] 异常, session_id={session_id}: {e}", flush=True)
        return None


def try_connect() -> Optional[XtQuantTrader]:
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


def ensure_connected() -> bool:
    global xt_trader
    if xt_trader is None:
        xt_trader = try_connect()
    return xt_trader is not None


# ================================================================
# RabbitMQ RPC Server
# ================================================================
async def rpc_server():
    global shutdown_event, _mq_conn, _mq_channel, _mq_exchange

    print(f"[RPC] Connecting to {RABBITMQ_URL}", flush=True)
    _mq_conn = await aio_pika.connect_robust(RABBITMQ_URL)
    _mq_channel = await _mq_conn.channel()
    await _mq_channel.set_qos(prefetch_count=1)

    _mq_exchange = await _mq_channel.declare_exchange(
        EXCHANGE_NAME, ExchangeType.TOPIC, durable=True,
    )

    req_queue = await _mq_channel.declare_queue(QUEUE_REQ, durable=True)
    await req_queue.bind(_mq_exchange, routing_key=QUEUE_REQ)
    print(f"[RPC] Connected, Listening on [{QUEUE_REQ}]", flush=True)

    # 声明并绑定推送队列，供客户端消费推送事件
    push_queue = await _mq_channel.declare_queue(QUEUE_PUSH, durable=True)
    await push_queue.bind(_mq_exchange, routing_key=QUEUE_PUSH)
    print(f"[RPC] Push queue ready: [{QUEUE_PUSH}]", flush=True)
    
    await asyncio.sleep(0.3)

    async with req_queue.iterator() as qiter:
        async for message in qiter:
            if shutdown_event.is_set():
                break

            async with message.process():
                try:
                    pkt = MsgPacket.decode(message.body)
                except Exception as e:
                    print(f"[RPC] Decode error: {e}", flush=True)
                    continue

                req_msg_id = pkt.msg_id().strip()
                req_func = pkt.func().strip('\x00')
                print(f"[RPC] <- {pkt.wire_to_string()}", flush=True)

                code, msg, data = handle_trade_request(pkt)
                #print(f"[RPC] -> {code}: {msg}", flush=True)

                ans = build_answer(pkt, req_msg_id, code, msg, data)
                print(f"[RPC] -> {ans.wire_to_string()}", flush=True)
                _, ans_wire = ans.encode()

                await _mq_channel.default_exchange.publish(
                    aio_pika.Message(body=ans_wire),
                    routing_key=QUEUE_REPLY,
                )
                #print(f"[RPC] -> reply to [{QUEUE_REPLY}], msg_id={req_msg_id}", flush=True)


# ================================================================
# 主程序
# ================================================================
def event_loop_thread():
    global loop, shutdown_event
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    shutdown_event = asyncio.Event()
    try:
        loop.run_until_complete(rpc_server())
    except asyncio.CancelledError:
        print("[Main] RPC server cancelled", flush=True)
    finally:
        print("[Main] Event loop exiting", flush=True)
        loop.close()


def process_events():
    while True:
        try:
            event_type, _ = event_queue.get(timeout=1)
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

    shutdown_flag = [False]

    def main_signal_handler(sig, frame):
        print("\n[Main] Shutdown signal received", flush=True)
        shutdown_flag[0] = True
        if shutdown_event:
            shutdown_event.set()

    try:
        signal.signal(signal.SIGINT, main_signal_handler)
        signal.signal(signal.SIGTERM, main_signal_handler)
    except ValueError:
        pass

    global xt_acc
    xt_acc = StockAccount(ACCOUNT_ID)
    print(f"[Main] 账户: {ACCOUNT_ID}", flush=True)

    if not ensure_connected():
        print("[Main] 初始连接失败，RPC server 仍会启动", flush=True)

    thread = threading.Thread(target=event_loop_thread, daemon=True)
    thread.start()
    print("[Main] RPC server 线程已启动", flush=True)

    try:
        while not shutdown_flag[0]:
            process_events()
    except KeyboardInterrupt:
        print("\n[Main] KeyboardInterrupt", flush=True)
        if shutdown_event:
            shutdown_event.set()


if __name__ == "__main__":
    main()
