"""
kafka_producer_demo.py — 使用 msgpacket 打包消息发送到 Kafka

依赖:
    pip install confluent-kafka

用法:
    python kafka_producer_demo.py [bootstrap_servers]
    默认 bootstrap_servers = localhost:9092
"""

import sys
import os
import json
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from msgpacket import (
    MsgPacket,
    MSG_TYPE_REQUEST, MSG_TYPE_PUSH, MSG_TYPE_ANSWER,
    MSG_FORMAT_TABLE,
)


def build_kline_msg(symbol: str, price: float, volume: float) -> bytes:
    """构建一个行情消息（K线数据），返回 wire 格式 bytes"""
    pkt = MsgPacket(MSG_TYPE_PUSH, "V1.0")
    pkt.set_func("kline")
    pkt.set_timestamp(time.strftime("%Y%m%d%H%M%S") + "%03d" % (int(time.time() * 1000) % 1000))

    # 表格式: symbol, price, volume, timestamp
    pkt.set_headers(4, "Symbol,Price,Volume,Timestamp")
    pkt.add_row()
    pkt.set_value("Symbol", symbol)
    pkt.set_value("Price", str(price))
    pkt.set_value("Volume", str(volume))
    pkt.set_value("Timestamp", pkt.timestamp())

    ret = pkt.finalize()
    if ret != 0:
        raise RuntimeError(f"msg_finalize failed: {ret}")

    # encode 得到完整 wire 数据（含 magic + crc32 + header + body）
    code, wire = pkt.encode()
    if code != 0:
        raise RuntimeError(f"msg_encode failed: {code}")
    return wire


def send_kafka(topic: str, bootstrap_servers: str, key: str, wire_data: bytes):
    """发送 wire 数据到 Kafka"""
    from confluent_kafka import Producer

    producer = Producer({"bootstrap.servers": bootstrap_servers})

    # 将 bytes 转为 base64 字符串方便传输（也可直接发 bytes，需配置 decoder）
    # 这里直接发 bytes，key 用字符串
    producer.produce(
        topic,
        key=key.encode("utf-8"),
        value=wire_data,
        on_delivery=_delivery_callback,
    )
    producer.flush()
    print(f"  [Sent] topic={topic}, key={key}, size={len(wire_data)} bytes")


def _delivery_callback(err, msg):
    if err:
        print(f"  [Delivery Error] {err}")
    else:
        print(f"  [Delivered] partition={msg.partition()}, offset={msg.offset()}")


def demo_produce():
    import argparse
    parser = argparse.ArgumentParser(description="MsgPacket Kafka Producer Demo")
    parser.add_argument("bootstrap_servers", nargs="?", default="localhost:9092",
                        help="Kafka bootstrap servers (default: localhost:9092)")
    parser.add_argument("--topic", default="msgpacket-demo",
                        help="Kafka topic (default: msgpacket-demo)")
    args = parser.parse_args()

    topic = args.topic
    bootstrap = args.bootstrap_servers

    print(f"=== MsgPacket Kafka Producer Demo ===")
    print(f"  bootstrap: {bootstrap}")
    print(f"  topic:     {topic}\n")

    # 发送多条测试消息
    messages = [
        ("BTC/USDT", 65000.50, 123.456),
        ("ETH/USDT", 3500.00, 789.012),
        ("SOL/USDT", 180.25, 2345.678),
    ]

    print(f"--- Sending {len(messages)} messages ---\n")
    for i, (sym, price, vol) in enumerate(messages):
        try:
            wire = build_kline_msg(sym, price, vol)
            send_kafka(topic, bootstrap, sym, wire)
        except Exception as e:
            print(f"  [ERROR] {e}")
        print()

    print("=== Producer done ===")


if __name__ == "__main__":
    demo_produce()
