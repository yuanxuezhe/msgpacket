"""
demo_builder.py — MsgPacket Python 多结果集打包示例

模仿 C 语言 demo_builder.c 的写法，演示如何构建含多个结果集的包，并解码验证。
"""

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from msgpacket import (
    MsgPacket,
    MSG_TYPE_REQUEST, MSG_TYPE_ANSWER,
    msg_type_name
)


# ================================================================
# 辅助：打印结果集信息
# ================================================================
def print_result_set_info(pkt: MsgPacket, rs_number: int):
    print(f"  Result Set {rs_number}:")
    print(f"    headers: {pkt.header_count()}, rows: {pkt.row_count()}")

    # 打印表头
    hdr_str = pkt.get_headers()
    print(f"    header: {hdr_str}")

    # 打印数据
    pkt.reset_cursor()
    while pkt.fetch_next():
        row = pkt.current_row()
        print(f"    row {row}:", end="")
        for col in range(pkt.header_count()):
            val = pkt.get_field(row, col)
            print(f" {val}", end="")
        print()
    pkt.reset_cursor()


# ================================================================
# 辅助：解码并遍历所有结果集
# ================================================================
def decode_and_print_all_rs(wire_data: bytes, wire_size: int):
    pkt = MsgPacket.decode(wire_data)

    print(f"  === Decoded Packet ===")
    print(f"  func: {pkt.func().strip()}  type: {chr(pkt.msg_type())}")
    print(f"  result_set_count: {pkt.result_set_count()}")

    # 遍历所有结果集，使用 next_result_set 切换
    print("  === Iterate all result sets ===")
    for rs in range(1, pkt.result_set_count() + 1):
        if rs > 1:
            if not pkt.next_result_set():
                print(f"  Failed to switch to RS{rs}")
                break
        print(f"  [RS{rs}] current_rs={pkt.result_set()}, rs_count={pkt.result_set_count()}, row_count={pkt.row_count()}")
        print_result_set_info(pkt, rs)


# ================================================================
# 主入口
# ================================================================
def main():
    print("=== MsgPacket Multi-Result-Set Python Demo ===\n")

    # ----------------------------------------------------------
    # 构建含多个结果集的包
    # ----------------------------------------------------------
    print("--- Build multi result-set packet ---\n")

    sender = MsgPacket(MSG_TYPE_REQUEST, "V1.0")
    sender.set_func("subscribe")
    sender.set_timestamp("20260501090101123")

    # RS1: 请求参数
    print("\n  Building RS1...")
    sender.set_headers(2, "Symbol,Price")
    sender.add_row()
    sender.set_value("Symbol", "BTC/USDT")
    sender.set_value("Price", "65000.50")

    sender.add_row()
    sender.set_value("Symbol", "ETH/USDT")
    sender.set_value("Price", "3500.00")

    # RS2: 附加信息
    print("  Adding RS2...")
    sender.add_result_set()
    sender.set_headers(2, "Tag,Note")
    sender.add_row()
    sender.set_value("Tag", "priority")
    sender.set_value("Note", "high-frequency")
    print(f"  RS2 row_count: {sender.row_count()}")

    # RS3: 扩展字段
    print("  Adding RS3...")
    sender.add_result_set()
    sender.set_headers(2, "Ext1,Ext2")
    sender.add_row()
    sender.add_row()
    sender.add_row()
    sender.set_value("Ext1", "ext_value_1")
    sender.set_value("Ext2", "ext_value_2")

    ret = sender.finalize()
    if ret != 0:
        print(f"  [ERROR] finalize failed: {ret}")
        return

    # 获取 wire 格式数据
    wire_data = sender.wire_data_bytes()
    wire_size = len(wire_data)

    print(f"\n  Wire data ({wire_size} bytes):")
    print(f"  {sender.wire_to_string()}")

    # ----------------------------------------------------------
    # 解码验证
    # ----------------------------------------------------------
    print("\n--- Decode and verify ---")
    decode_and_print_all_rs(wire_data, wire_size)

    print("\n=== Python Demo completed successfully ===")


if __name__ == "__main__":
    main()
