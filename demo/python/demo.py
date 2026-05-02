"""
demo.py — MsgPacket Python Complete Request-Answer Demo

Uses ctypes bindings to demonstrate:
  Request build -> encode -> decode -> Answer build -> decode
All output is human-readable (no hex dumps).
"""

import sys
import os
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from msgpacket import (
    MsgPacket,
    MSG_TYPE_REQUEST, MSG_TYPE_ANSWER,
    MSG_CODE_SUCCESS,
    HEAD_SIZE, BODY_OFFSET, msg_type_name
)


# ================================================================
# Helpers
# ================================================================
def print_body_raw(pkt: MsgPacket):
    data = pkt.wire_data_bytes()
    if not data:
        print("  (no wire data)")
        return
    body = data[BODY_OFFSET : BODY_OFFSET + pkt.body_len()]
    print(f"  Body raw ({len(body)} bytes): ", end="")
    for b in body:
        if b == 0x1F:
            print("<US>", end="")
        elif b == 0x1E:
            print("<RS>", end="")
        elif b == 0x1C:
            print("<FS>", end="")
        elif b == 0x1B:
            print("<ESC>", end="")
        elif 0x20 <= b < 0x7F:
            print(chr(b), end="")
        else:
            print("#", end="")
    print()


def print_table(pkt: MsgPacket):
    hdr_cnt = pkt.header_count()
    if hdr_cnt == 0:
        print("  (no headers)")
        return

    # Get header names
    hdr_str = pkt.get_headers()
    header_names = [h.strip() for h in hdr_str.split(",")]
    col_widths = [len(h) for h in header_names]

    # Scan data for column widths
    pkt.reset_cursor()
    while pkt.fetch_next():
        for col in range(hdr_cnt):
            val = pkt.get_field(pkt.current_row(), col)
            if len(val) > col_widths[col]:
                col_widths[col] = len(val)
    pkt.reset_cursor()

    # Print header row
    print("  | ", end="")
    for i in range(hdr_cnt):
        print(f"{header_names[i]:<{col_widths[i]}}", end="")
        if i < hdr_cnt - 1:
            print(" | ", end="")
    print(" |")

    # Separator
    print("  |-", end="")
    for i in range(hdr_cnt):
        print("-" * col_widths[i], end="")
        if i < hdr_cnt - 1:
            print("-+-", end="")
    print("-|")

    # Data rows
    while pkt.fetch_next():
        print("  | ", end="")
        for col in range(hdr_cnt):
            val = pkt.get_field(pkt.current_row(), col)
            print(f"{val:<{col_widths[col]}}", end="")
            if col < hdr_cnt - 1:
                print(" | ", end="")
        print(" |")
    pkt.reset_cursor()


def print_header(pkt: MsgPacket):
    ts = pkt.timestamp()
    if ts and len(ts) >= 17:
        dt = f"{ts[0:4]}-{ts[4:6]}-{ts[6:8]} {ts[8:10]}:{ts[10:12]}:{ts[12:14]}.{ts[14:17]}"
    else:
        dt = "N/A"
    print(f"  msg_id:    {pkt.msg_id()}")
    print(f"  version:   {pkt.version()}")
    print(f"  func:      {pkt.func()}")
    print(f"  type:      {msg_type_name(pkt.msg_type())} ({chr(pkt.msg_type())})")
    fmt = pkt.format()
    print(f"  format:    {fmt} '{chr(fmt)}'")
    print(f"  code:      {pkt.code()}")
    print(f"  timestamp: {ts} ({dt})")
    print(f"  body_len:  {pkt.body_len()} bytes")
    print(f"  total_len: {pkt.total_len()} bytes")


# ================================================================
# Main
# ================================================================
def main():
    print("=" * 60)
    print("  MsgPacket Python Demo - Request/Answer Full Cycle")
    print("=" * 60)

    # ------------------------------------------------------------
    # Phase 1: Client builds REQUEST
    # ------------------------------------------------------------
    print("\n--- Phase 1: Client Builds REQUEST ---\n")

    request = MsgPacket(MSG_TYPE_REQUEST, "V1.0")
    request.set_func("getData")
    request.set_code(MSG_CODE_SUCCESS)
    request.set_timestamp(None)
    request.set_headers(4, "Symbol,Price,Volume,Time")

    request.begin_row()
    request.set_value("Symbol", "BTC/USDT")
    request.set_value("Price", "65000.50")
    request.set_value("Volume", 1.2)
    request.set_value("Time", 1717000000000)

    request.begin_row()
    request.set_value("Symbol", "ETH/USDT")
    request.set_value("Price", "3500.00")
    request.set_value("Volume", 10.5)
    request.set_value("Time", 1717000000000)

    request.begin_row()
    request.set_value("Symbol", "SOL/USDT")
    request.set_value("Price", "150.00")
    request.set_value("Volume", 100.0)
    request.set_value("Time", 1717000000000)

    ret = request.finalize()
    if ret != 0:
        print(f"  [ERROR] finalize failed: {ret}")
        return

    print_header(request)

    magic = request.wire_data_bytes()[:4].decode('utf-8', errors='replace')
    print(f"\n  Magic: '{magic}' [OK]")

    print(f"\n  Request Body (raw with markers):")
    print_body_raw(request)

    print(f"\n  Request Body (table view):")
    display = MsgPacket.decode(request.wire_data_bytes())
    print_table(display)

    print(f"\n  [OK] Built request: {request.wire_size()} bytes")

    # ------------------------------------------------------------
    # Phase 2: Server decodes REQUEST
    # ------------------------------------------------------------
    print("\n--- Phase 2: Server Decodes REQUEST ---\n")

    received_req = MsgPacket.decode(request.wire_data_bytes())

    print(f"  [OK] CRC verified")
    print(f"  Received: func=[{received_req.func()}] "
          f"type=[{chr(received_req.msg_type())}] "
          f"code=[{received_req.code()}]")

    print(f"\n  Processing request symbols:")
    while received_req.fetch_next():
        sym = received_req.get_value_str("Symbol")
        print(f"    Query: {sym}")

    # ------------------------------------------------------------
    # Phase 3: Server builds ANSWER
    # ------------------------------------------------------------
    print("\n--- Phase 3: Server Builds ANSWER ---\n")

    answer = MsgPacket(MSG_TYPE_ANSWER, "V1.0")
    answer.set_func("getData")
    answer.set_code(MSG_CODE_SUCCESS)
    answer.set_timestamp(None)
    answer.set_headers(4, "Symbol,Price,Volume,Time")

    answer.begin_row()
    answer.set_value("Symbol", "BTC/USDT")
    answer.set_value("Price", "65500.00")
    answer.set_value("Volume", 2.5)
    answer.set_value("Time", 1717000001000)

    answer.begin_row()
    answer.set_value("Symbol", "ETH/USDT")
    answer.set_value("Price", "3520.00")
    answer.set_value("Volume", 15.8)
    answer.set_value("Time", 1717000001000)

    answer.begin_row()
    answer.set_value("Symbol", "SOL/USDT")
    answer.set_value("Price", "152.00")
    answer.set_value("Volume", 200.0)
    answer.set_value("Time", 1717000001000)

    answer.finalize()

    print_header(answer)

    print(f"\n  Answer Body (raw with markers):")
    print_body_raw(answer)

    print(f"\n  Answer Body (table view):")
    display = MsgPacket.decode(answer.wire_data_bytes())
    print_table(display)

    print(f"\n  [OK] Built answer: {answer.wire_size()} bytes")

    # ------------------------------------------------------------
    # Phase 4: Client decodes ANSWER
    # ------------------------------------------------------------
    print("\n--- Phase 4: Client Decodes ANSWER ---\n")

    received_ans = MsgPacket.decode(answer.wire_data_bytes())

    print(f"  [OK] CRC verified")
    print(f"  Received: func=[{received_ans.func()}] "
          f"type=[{chr(received_ans.msg_type())}] "
          f"code=[{received_ans.code()}]")

    print(f"\n  Answer Data (table view):")
    print_table(received_ans)

    print(f"\n  Answer Data (raw with markers):")
    print_body_raw(received_ans)

    print(f"\n{'=' * 60}")
    print(f"  Python Demo completed successfully!")
    print(f"{'=' * 60}")


if __name__ == "__main__":
    main()
