/**
 * demo_builder.cpp — MsgPacket C++ 多结果集打包示例
 *
 * 演示如何构建含多个结果集的包，并解码验证。
 * 模仿 C 语言 demo_builder.c 的写法。
 */

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <cstring>
#include <cctype>
#include <ctime>
#include <sstream>
#include "msg_packet.hpp"

using namespace msgpacket;

/* ================================================================
 * 辅助：打印结果集信息
 * ================================================================ */
static void print_result_set_info(MsgPacket& pkt, size_t rs_number)
{
    std::cout << "  Result Set " << rs_number << ":\n";
    std::cout << "    headers: " << pkt.header_count()
              << ", rows: " << pkt.row_count() << "\n";

    /* 打印表头 */
    char hdr_buf[4096] = {0};
    size_t hdr_len = sizeof(hdr_buf);
    msg_get_headers(pkt.c_ptr(), hdr_buf, &hdr_len);
    std::cout << "    header: " << hdr_buf << "\n";

    /* 打印数据 */
    pkt.reset_cursor();
    while (pkt.fetch_next()) {
        size_t row = pkt.current_row();
        std::cout << "    row " << row << ":";
        for (size_t col = 0; col < pkt.header_count(); col++) {
            auto val = pkt.get_field(row, col);
            std::cout << " " << val;
        }
        std::cout << "\n";
    }
    pkt.reset_cursor();
}

/* ================================================================
 * 辅助：解码并遍历所有结果集
 * ================================================================ */
static void decode_and_print_all_rs(const void* wire_data, size_t wire_size)
{
    MsgPacket pkt;
    try {
        pkt = MsgPacket::decode(wire_data, wire_size);
    } catch (const std::exception& e) {
        std::cout << "  Decode failed: " << e.what() << "\n";
        return;
    }

    std::cout << "  === Decoded Packet ===\n";
    char func_buf[9] = {0};
    msg_get_func(pkt.c_ptr(), func_buf);
    std::cout << "  func: " << func_buf
              << "  type: " << (char)pkt.type()
              << "  code: ";
    char code_buf[6] = {0};
    memcpy(code_buf, msg_get_code(pkt.c_ptr()), 5);
    std::cout << code_buf << "\n";
    std::cout << "  result_set_count: " << msg_get_result_set_count(pkt.c_ptr()) << "\n";

    /* 遍历所有结果集，使用 msg_next_result_set 切换 */
    std::cout << "  === Iterate all result sets ===\n";
    for (size_t rs = 1; rs <= msg_get_result_set_count(pkt.c_ptr()); rs++) {
        if (rs > 1) {
            if (!msg_next_result_set(pkt.c_ptr())) {
                std::cout << "  Failed to switch to RS" << rs << "\n";
                break;
            }
        }
        std::cout << "  [RS" << rs << "] current_rs=" << msg_get_result_set(pkt.c_ptr())
                  << ", rs_count=" << msg_get_result_set_count(pkt.c_ptr())
                  << ", row_count=" << pkt.row_count() << "\n";
        print_result_set_info(pkt, rs);
    }
}

/* ================================================================
 * 主入口
 * ================================================================ */
int main()
{
    std::cout << "=== MsgPacket Multi-Result-Set C++ Demo ===\n\n";

    /* ----------------------------------------------------------
     * 构建含多个结果集的包
     * ---------------------------------------------------------- */
    std::cout << "--- Build multi result-set packet ---\n";

    MsgPacket sender(MsgType::REQUEST, "V1.0");
    sender.set_func("subscribe");
    sender.set_timestamp("20260501090101123");

    /* RS1: 请求参数 */
    std::cout << "\n  Building RS1...\n";
    sender.set_headers(2, "Symbol,Price");
    sender.add_row();
    sender.set_value("Symbol", "BTC/USDT");
    sender.set_value("Price", "65000.50");

    sender.add_row();
    sender.set_value("Symbol", "ETH/USDT");
    sender.set_value("Price", "3500.00");

    /* RS2: 附加信息 */
    std::cout << "  Adding RS2...\n";
    if (!msg_add_result_set(sender.c_ptr())) {
        std::cout << "  msg_add_result_set failed\n";
        return 1;
    }
    sender.set_headers(2, "Tag,Note");
    int rc2 = msg_set_row(sender.c_ptr(), "%s,%s", "priority", "high-frequency");
    std::cout << "  msg_set_row for RS2 returned: " << rc2 << "\n";

    /* RS3: 扩展字段 */
    std::cout << "  Adding RS3...\n";
    if (!msg_add_result_set(sender.c_ptr())) {
        std::cout << "  msg_add_result_set failed\n";
        return 1;
    }
    sender.set_headers(2, "Ext1,Ext2");
    sender.add_row();
    sender.add_row();
    sender.add_row();
    sender.set_value("Ext1", "ext_value_1");
    sender.set_value("Ext2", "ext_value_2");

    if (sender.finalize() != 0) {
        std::cout << "  [ERROR] finalize failed\n";
        return 1;
    }

    /* 获取 wire 格式数据 */
    const void* wire_data = sender.wire_data();
    size_t wire_size = sender.wire_size();

    char* sender_str = msg_wire_to_string(sender.c_ptr());
    if (sender_str) {
        std::cout << "\n  Wire data (" << wire_size << " bytes):\n  " << sender_str << "\n";
        msg_free_buffer(sender_str);
    }

    /* ----------------------------------------------------------
     * 解码验证
     * ---------------------------------------------------------- */
    std::cout << "\n--- Decode and verify ---\n";
    decode_and_print_all_rs(wire_data, wire_size);

    std::cout << "\n=== C++ Demo completed successfully ===\n";
    return 0;
}