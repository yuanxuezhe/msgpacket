#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
XtQuant API + msgpacket RPC Server

整合 QMT 交易接口和 RabbitMQ RPC，支持：
1. 从 RabbitMQ 接收交易请求（查持仓/订单/资产等），处理后返回应答
2. 将 QMT 回调事件（成交/委托/错误）推送到 RabbitMQ

用法:
    python xtquant_api.py
"""

import asyncio
import random
import threading
import time
from datetime import datetime
from queue import Empty, Queue
from typing import Callable, Dict, List, Optional, Tuple

import aio_pika
from aio_pika import ExchangeType

from msgpacket import MsgPacket, MSG_TYPE_ANSWER, MSG_TYPE_PUSH
from xtquant.xttrader import XtQuantTrader, XtQuantTraderCallback
from xtquant.xttype import StockAccount
from xtquant import xtconstant


# ================================================================
# 配置
# ================================================================

class Config:
    """全局配置"""
    RABBITMQ_URL = "amqp://192.168.10.2:5672/"
    EXCHANGE_NAME = "msgpacket.exchange"
    QUEUE_REQ = "EvTrade.Req"
    QUEUE_REPLY = "EvTrade.Reply"
    QUEUE_PUSH = "EvTrade.Push"
    ACCOUNT_PATH = r"D:\software\trade\iQuant\userdata"
    ACCOUNT_ID = "410001265100"


config = Config()


# ================================================================
# 全局状态
# ================================================================

class GlobalState:
    """全局共享状态"""
    def __init__(self):
        self.xt_trader = None
        self.xt_acc = None
        self.event_queue = Queue()
        self.loop = None
        self.shutdown_event = None
        # RabbitMQ 长连接
        self.mq_conn = None
        self.mq_channel = None
        self.mq_exchange = None


state = GlobalState()


# ================================================================
# Handler 注册
# ================================================================
#: Return: (code: str, msg: str, data: Optional[List[Dict]])
HandlerReturn = Tuple[str, str, Optional[List[Dict]]]
HandlerFunc = Callable[[MsgPacket], HandlerReturn]

_HANDLERS: Dict[str, HandlerFunc] = {}


def handler(func_name: str) -> Callable[[HandlerFunc], HandlerFunc]:
    """Handler 注册装饰器"""
    def decorator(func: HandlerFunc) -> HandlerFunc:
        _HANDLERS[func_name] = func
        return func
    return decorator


def handle_trade_request(pkt: MsgPacket) -> HandlerReturn:
    """分发交易请求到对应 handler"""
    func = pkt.func().strip('\x00')
    if state.xt_trader is None:
        return "99999", "交易接口未连接", None
    handler = _HANDLERS.get(func)
    if handler is None:
        return "99999", f"unknown func: {func}", None
    try:
        return handler(pkt)
    except Exception as e:
        return "99999", str(e), None


# ================================================================
# 查询类 Handler
# ================================================================

@handler("qry_pos")
def _h_qry_pos(_pkt: MsgPacket) -> HandlerReturn:
    """查询持仓"""
    positions = state.xt_trader.query_stock_positions(state.xt_acc)
    return "00000", "ok", [{
        "stock_code": pos.stock_code,
        "volume": pos.volume,
        "avl_amt": pos.can_use_volume,
        "avg_price": pos.open_price,
        "market_value": pos.market_value,
    } for pos in positions]


@handler("qry_ord")
def _h_qry_ord(_pkt: MsgPacket) -> HandlerReturn:
    """查询订单"""
    orders = state.xt_trader.query_stock_orders(state.xt_acc)
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


@handler("qry_ast")
def _h_qry_ast(_pkt: MsgPacket) -> HandlerReturn:
    """查询资产"""
    asset = state.xt_trader.query_stock_asset(state.xt_acc)
    if asset is None:
        return "99999", "查询资产失败", None
    return "00000", "ok", [{
        "account_id": asset.account_id,
        "cash": asset.cash,
        "frozen_cash": asset.frozen_cash,
        "market_value": asset.market_value,
        "total_asset": asset.total_asset,
    }]


@handler("qry_mch")
def _h_qry_mch(_pkt: MsgPacket) -> HandlerReturn:
    """查询成交"""
    trades = state.xt_trader.query_stock_trades(state.xt_acc)
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


# ================================================================
# 交易类 Handler
# ================================================================

@handler("ord_stk")
def _h_ord_stk(pkt: MsgPacket) -> HandlerReturn:
    """异步下单"""
    stock_code = pkt.get_value_str("stock_code")
    volume = int(pkt.get_value_str("volume"))
    price_type_str = pkt.get_value_str("price_type")
    price = float(pkt.get_value_str("price"))
    direction_str = pkt.get_value_str("direction")
    remark = pkt.get_value_str("remark")

    price_type_map = {"1": xtconstant.LATEST_PRICE, "0": xtconstant.FIX_PRICE}
    price_type = price_type_map.get(price_type_str, xtconstant.LATEST_PRICE)
    direction = xtconstant.STOCK_BUY if direction_str == "BUY" else xtconstant.STOCK_SELL

    seq = state.xt_trader.order_stock_async(
        state.xt_acc, stock_code, direction, volume,
        price_type, price, "xtquant_api", remark,
    )
    return "00000", "ok", [{"seq": seq}]


@handler("cxl_ord")
def _h_cxl_ord(pkt: MsgPacket) -> HandlerReturn:
    """撤单"""
    order_id = pkt.get_value_str("order_id")
    market_str = pkt.get_value_str("market")
    market = xtconstant.SZ_MARKET if market_str == "SZ" else xtconstant.SH_MARKET
    result = state.xt_trader.cancel_order_stock_async(state.xt_acc, market, order_id)
    return "00000", "ok", [{"result": result}]


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
    """QMT 交易回调，转事件到 RabbitMQ"""

    def on_disconnected(self) -> None:
        print(f"[{datetime.now().strftime('%H:%M:%S')}] [Cb] 连接断开，将重连", flush=True)
        state.xt_trader = None
        state.event_queue.put(("disconnected", None))

    def on_stock_order(self, order) -> None:
        """委托确认"""
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

    def on_stock_trade(self, trade) -> None:
        """成交回报"""
        push_event("trd_cfm", [{
            "traded_id": trade.traded_id,
            "stock_code": trade.stock_code,
            "traded_volume": trade.traded_volume,
            "traded_price": trade.traded_price,
            "account_id": trade.account_id,
            "strategy_name": trade.strategy_name,
            "remark": trade.order_remark,
        }])

    def on_order_error(self, order_error) -> None:
        """报单失败"""
        push_event("ord_err", [{
            "order_id": order_error.order_id,
            "error_msg": order_error.error_msg,
        }])

    def on_cancel_error(self, cancel_error) -> None:
        """撤单失败"""
        push_event("cxl_err", [{
            "order_id": cancel_error.order_id,
            "error_msg": cancel_error.error_msg,
        }])

    def on_order_stock_async_response(self, response) -> None:
        """异步下单响应"""
        push_event("ord_ack", [{
            "seq": response.seq,
            "order_id": response.order_sysid,
        }])

    def on_account_status(self, status) -> None:
        """账号状态"""
        push_event("acc_sts", [{
            "account_id": status.account_id,
            "status": status.status,
        }])


# ================================================================
# RabbitMQ 推送
# ================================================================

def push_event(func: str, data: List[Dict]) -> None:
    """线程安全地推送事件到 RabbitMQ"""
    if state.loop is None or state.loop.is_closed():
        return
    asyncio.run_coroutine_threadsafe(_mq_publish(func, data), state.loop)


async def _mq_publish(func: str, data: List[Dict]) -> None:
    """推送消息: func=功能号, data=RS1数据表"""
    if state.mq_exchange is None:
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
        await state.mq_exchange.publish(aio_pika.Message(body=wire), routing_key=config.QUEUE_PUSH)
    except Exception as e:
        print(f"[Push] 失败 {func}: {e}", flush=True)


# ================================================================
# XtQuantTrader 连接管理
# ================================================================

def create_trader(session_id: int) -> Optional[XtQuantTrader]:
    """创建并连接交易客户端"""
    try:
        callback = MyXtQuantTraderCallback()
        trader = XtQuantTrader(config.ACCOUNT_PATH, session_id, callback=callback)
        trader.start()
        result = trader.connect()
        if result == 0:
            trader.subscribe(state.xt_acc)
            print(f"[Trader] 连接成功, session_id={session_id}", flush=True)
            return trader
        print(f"[Trader] 连接失败, session_id={session_id}, result={result}", flush=True)
        return None
    except Exception as e:
        print(f"[Trader] 异常, session_id={session_id}: {e}", flush=True)
        return None


def try_connect() -> Optional[XtQuantTrader]:
    """尝试连接，使用随机 session_id"""
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
    """确保交易接口已连接"""
    if state.xt_trader is None:
        state.xt_trader = try_connect()
    return state.xt_trader is not None


# ================================================================
# RabbitMQ RPC Server
# ================================================================

async def rpc_server() -> None:
    """RPC 服务器：接收请求、调用 handler、返回应答"""
    print(f"[RPC] Connecting to {config.RABBITMQ_URL}", flush=True)
    state.mq_conn = await aio_pika.connect_robust(config.RABBITMQ_URL)
    state.mq_channel = await state.mq_conn.channel()
    await state.mq_channel.set_qos(prefetch_count=1)

    state.mq_exchange = await state.mq_channel.declare_exchange(
        config.EXCHANGE_NAME, ExchangeType.TOPIC, durable=True,
    )

    req_queue = await state.mq_channel.declare_queue(config.QUEUE_REQ, durable=True)
    await req_queue.bind(state.mq_exchange, routing_key=config.QUEUE_REQ)
    print(f"[RPC] Connected, Listening on [{config.QUEUE_REQ}]", flush=True)

    # 声明并绑定推送队列，供客户端消费推送事件
    push_queue = await state.mq_channel.declare_queue(config.QUEUE_PUSH, durable=True)
    await push_queue.bind(state.mq_exchange, routing_key=config.QUEUE_PUSH)
    print(f"[RPC] Push queue ready: [{config.QUEUE_PUSH}]", flush=True)

    await asyncio.sleep(0.3)

    async with req_queue.iterator() as qiter:
        async for message in qiter:
            if state.shutdown_event.is_set():
                break

            async with message.process():
                await _handle_message(message)


async def _handle_message(message) -> None:
    """处理单条 RPC 请求"""
    try:
        pkt = MsgPacket.decode(message.body)
    except Exception as e:
        print(f"[RPC] Decode error: {e}", flush=True)
        return

    req_msg_id = pkt.msg_id().strip()
    print(f"[RPC] <- {pkt.wire_to_string()}", flush=True)

    code, msg, data = handle_trade_request(pkt)

    ans = build_answer(pkt, req_msg_id, code, msg, data)
    print(f"[RPC] -> {ans.wire_to_string()}", flush=True)
    _, ans_wire = ans.encode()

    await state.mq_channel.default_exchange.publish(
        aio_pika.Message(body=ans_wire),
        routing_key=config.QUEUE_REPLY,
    )


# ================================================================
# 主程序
# ================================================================

def event_loop_thread() -> None:
    """运行 asyncio 事件循环的线程"""
    state.loop = asyncio.new_event_loop()
    asyncio.set_event_loop(state.loop)
    state.shutdown_event = asyncio.Event()
    try:
        state.loop.run_until_complete(rpc_server())
    except asyncio.CancelledError:
        print("[Main] RPC server cancelled", flush=True)
    finally:
        print("[Main] Event loop exiting", flush=True)
        state.loop.close()


def main() -> None:
    """主入口"""
    state.xt_acc = StockAccount(config.ACCOUNT_ID)
    print(f"[Main] 账户: {config.ACCOUNT_ID}", flush=True)

    if not ensure_connected():
        print("[Main] 初始连接失败，RPC server 仍会启动", flush=True)

    threading.Thread(target=event_loop_thread, daemon=True).start()
    print("[Main] RPC server 线程已启动", flush=True)
    
    # 阻塞主线程退出
    state.xt_trader.run_forever()

if __name__ == "__main__":
    print("=" * 60)
    print("XtQuant API + msgpacket RPC Server")
    print("=" * 60)
    main()
