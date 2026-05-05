"""
kafka_consumer_demo.py — 从 Kafka 订阅消息并用 msgpacket 解包

依赖:
    pip install confluent-kafka

用法:
    python kafka_consumer_demo.py [bootstrap_servers]
    默认 bootstrap_servers = localhost:9092
"""

import sys
import os
import signal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from msgpacket import MsgPacket


def decode_and_print(wire_data: bytes) -> bool:
    """
    解包 wire 数据并打印内容
    返回 True 表示解包成功
    """
    try:
        pkt = MsgPacket.decode(wire_data)
    except RuntimeError as e:
        print(f"  [Decode Error] {e}")
        return False

    print(f"  === Decoded Packet ===")
    print(f"  func:      {pkt.func().strip()}")
    print(f"  msg_type:  {pkt.msg_type_name()} (0x{pkt.msg_type():02X})")
    print(f"  version:   {pkt.version().strip()}")
    print(f"  timestamp: {pkt.timestamp()}")
    print(f"  headers:   {pkt.get_headers()}")
    print(f"  row_count: {pkt.row_count()}")

    # 遍历数据行
    pkt.reset_cursor()
    while pkt.fetch_next():
        row = pkt.current_row()
        print(f"  row {row}:", end="")
        for col in range(pkt.header_count()):
            val = pkt.get_field(row, col)
            print(f" {val}", end="")
        print()
    pkt.reset_cursor()

    return True


def consume_kafka(topic: str, bootstrap_servers: str, group_id: str):
    """从 Kafka 消费消息"""
    from confluent_kafka import Consumer

    consumer = Consumer({
        "bootstrap.servers": bootstrap_servers,
        "group.id": group_id,
        "auto.offset.reset": "earliest",
        "enable.auto.commit": True,
    })

    consumer.subscribe([topic])
    print(f"  subscribed to topic: {topic}")

    running = True

    def signal_handler(signum, frame):
        nonlocal running
        running = False
        print("\n  [Signal] Shutting down...")

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    print(f"  waiting for messages (Ctrl+C to exit)...\n")

    msg_count = 0
    while running:
        msg = consumer.poll(timeout=1.0)
        if msg is None:
            continue
        if msg.error():
            print(f"  [Consumer Error] {msg.error()}")
            continue

        msg_count += 1
        wire_data = msg.value()
        key = msg.key().decode("utf-8") if msg.key() else "(none)"

        print(f"--- Message #{msg_count} ---")
        print(f"  partition={msg.partition()}, offset={msg.offset()}, key={key}")
        print(f"  size={len(wire_data)} bytes")
        decode_and_print(wire_data)
        print()

    consumer.close()
    print(f"[Consumer] Exited. Total messages processed: {msg_count}")


def demo_consume():
    import argparse
    parser = argparse.ArgumentParser(description="MsgPacket Kafka Consumer Demo")
    parser.add_argument("bootstrap_servers", nargs="?", default="localhost:9092",
                        help="Kafka bootstrap servers (default: localhost:9092)")
    parser.add_argument("--topic", default="msgpacket-demo",
                        help="Kafka topic (default: msgpacket-demo)")
    parser.add_argument("--group", default="msgpacket-consumer-group",
                        help="Consumer group ID (default: msgpacket-consumer-group)")
    args = parser.parse_args()

    print(f"=== MsgPacket Kafka Consumer Demo ===")
    print(f"  bootstrap: {args.bootstrap_servers}")
    print(f"  topic:     {args.topic}")
    print(f"  group:     {args.group}\n")

    try:
        consume_kafka(args.topic, args.bootstrap_servers, args.group)
    except KeyboardInterrupt:
        pass
    print("=== Consumer done ===")


if __name__ == "__main__":
    demo_consume()
