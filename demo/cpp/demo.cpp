/**
 * demo.cpp — MsgPacket C++ 完整请求-应答 Demo
 *
 * 使用 RAII 封装类 msgpacket::MsgPacket，
 * 完整演示请求包构建 → 编码 → 解码 → 应答包构建 → 解码的流程。
 * 所有输出均为肉眼可读的字符格式。
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
 * 辅助：可读 Body 打印
 * ================================================================ */
static void print_body_raw(const MsgPacket& pkt)
{
    auto* data = static_cast<const uint8_t*>(pkt.wire_data());
    if (!data) return;
    const uint8_t* body = data + BODY_OFFSET;
    uint32_t len = pkt.body_len();

    std::cout << "  Body raw (" << len << " bytes): ";
    for (uint32_t i = 0; i < len; i++) {
        switch (body[i]) {
        case 0x1F: std::cout << "<US>";  break;
        case 0x1E: std::cout << "<RS>";  break;
        case 0x1C: std::cout << "<FS>";  break;
        case 0x1B: std::cout << "<ESC>"; break;
        default:
            if (std::isprint(body[i]))
                std::cout << body[i];
            else
                std::cout << '#';
            break;
        }
    }
    std::cout << std::endl;
}

/* ================================================================
 * 辅助：表格视图
 * ================================================================ */
static void print_table(const MsgPacket& pkt)
{
    size_t hdr_cnt = pkt.header_count();
    if (hdr_cnt == 0) {
        std::cout << "  (no headers)" << std::endl;
        return;
    }

    /* 获取表头名称 */
    std::string hdr_str = pkt.get_headers();
    std::vector<std::string> header_names;
    std::vector<size_t> col_widths;

    size_t pos = 0;
    while (pos < hdr_str.size()) {
        size_t comma = hdr_str.find(',', pos);
        std::string name = hdr_str.substr(pos, comma - pos);
        /* trim leading spaces */
        while (!name.empty() && name[0] == ' ') name.erase(0, 1);
        header_names.push_back(name);
        col_widths.push_back(name.size());
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }

    /* 扫描数据计算列宽 */
    {
        /* 需要非 const 访问游标，所以 const_cast。
         * 实际上 fetch_next/reset_cursor 修改内部游标但不影响数据。
         */
        MsgPacket& mut = const_cast<MsgPacket&>(pkt);
        mut.reset_cursor();
        while (mut.fetch_next()) {
            for (size_t col = 0; col < hdr_cnt; col++) {
                auto val = pkt.get_field(pkt.current_row(), col);
                if (val.size() > col_widths[col])
                    col_widths[col] = val.size();
            }
        }
        mut.reset_cursor();
    }

    /* 打印表头 */
    std::cout << "  | ";
    for (size_t i = 0; i < hdr_cnt; i++) {
        std::cout << std::left << std::setw((int)col_widths[i]) << header_names[i];
        if (i < hdr_cnt - 1) std::cout << " | ";
    }
    std::cout << " |" << std::endl;

    /* 分隔线 */
    std::cout << "  |-";
    for (size_t i = 0; i < hdr_cnt; i++) {
        std::cout << std::string(col_widths[i], '-');
        if (i < hdr_cnt - 1) std::cout << "-+-";
    }
    std::cout << "-|" << std::endl;

    /* 打印数据行 */
    MsgPacket& mut = const_cast<MsgPacket&>(pkt);
    while (mut.fetch_next()) {
        std::cout << "  | ";
        for (size_t col = 0; col < hdr_cnt; col++) {
            auto val = pkt.get_field(pkt.current_row(), col);
            std::cout << std::left << std::setw((int)col_widths[col]) << val;
            if (col < hdr_cnt - 1) std::cout << " | ";
        }
        std::cout << " |" << std::endl;
    }
    mut.reset_cursor();
}

/* ================================================================
 * 辅助：打印 Header
 * ================================================================ */
static void print_header(const MsgPacket& pkt, const std::string& label)
{
    std::cout << "  --- " << label << " ---" << std::endl;
    std::cout << "  msg_id:    " << pkt.msg_id() << std::endl;
    std::cout << "  version:   " << pkt.version() << std::endl;
    std::cout << "  func:      " << pkt.func() << std::endl;
    std::cout << "  type:      " << msg_type_name(static_cast<MsgType>(pkt.type()))
              << " (" << static_cast<char>(pkt.type()) << ")" << std::endl;
    std::cout << "  format:    " << (int)pkt.format() << " '"
              << (char)pkt.format() << "'" << std::endl;

    /* timestamp: yyyyMMddHHmmssSSS (17 chars) */
    {
        std::string ts = pkt.timestamp();
        std::cout << "  timestamp: " << ts << " ("
                  << ts.substr(0, 4) << "-" << ts.substr(4, 2) << "-" << ts.substr(6, 2)
                  << " " << ts.substr(8, 2) << ":" << ts.substr(10, 2) << ":"
                  << ts.substr(12, 2) << "." << ts.substr(14, 3) << ")"
                  << std::endl;
    }
    std::cout << "  body_len:  " << pkt.body_len() << " bytes" << std::endl;
    std::cout << "  total_len: " << pkt.total_len() << " bytes" << std::endl;
}

/* ================================================================
 * 辅助：本地解码以显示
 * ================================================================ */
static MsgPacket local_decode(const MsgPacket& pkt)
{
    auto* data = static_cast<const uint8_t*>(pkt.wire_data());
    return MsgPacket::decode(data, pkt.wire_size());
}

/* ================================================================
 * 主入口
 * ================================================================ */
int main()
{
    std::cout << "=== MsgPacket C++ RAII Demo ===" << std::endl;
    std::cout << "  (Complete Request-Answer Cycle)" << std::endl;
    std::cout << std::endl;

    try {
        /* ========================================================
         * Phase 1: Client 构建 Request 包
         * ======================================================== */
        std::cout << "--- Phase 1: Client Builds REQUEST ---" << std::endl;
        std::cout << std::endl;

        MsgPacket request(MsgType::REQUEST, "V1.0");
        request.set_func("getData");
        request.set_timestamp(nullptr);
        request.set_headers(4, "Symbol,Price,Volume,Time");

        /* Key-Value API */
        request.add_row();
        request.set_value("Symbol", "BTC/USDT");
        request.set_value("Price", "65000.50");
        request.set_value("Volume", 1.2);
        request.set_value("Time", (int64_t)1717000000000LL);

        request.add_row();
        request.set_value("Symbol", "ETH/USDT");
        request.set_value("Price", "3500.00");
        request.set_value("Volume", 10.5);
        request.set_value("Time", (int64_t)1717000000000LL);

        request.add_row();
        request.set_value("Symbol", "SOL/USDT");
        request.set_value("Price", "150.00");
        request.set_value("Volume", 100.0);
        request.set_value("Time", (int64_t)1717000000000LL);

        if (request.finalize() != 0) {
            std::cerr << "Finalize request failed!" << std::endl;
            return 1;
        }

        print_header(request, "REQUEST Packet Header");
        std::cout << std::endl;

        std::cout << "  Request Body (raw with markers):" << std::endl;
        print_body_raw(request);

        std::cout << std::endl;
        std::cout << "  Request Body (table view):" << std::endl;
        {
            auto display = local_decode(request);
            print_table(display);
        }
        std::cout << std::endl;

        std::cout << "  ✓ Magic: '" << std::string((const char*)request.wire_data(), 4)
                  << "' — verified" << std::endl;
        std::cout << "  ✓ Wire size: " << request.wire_size() << " bytes" << std::endl;

        /* ========================================================
         * Phase 2: 模拟传输 + Server 解码
         * ======================================================== */
        std::cout << std::endl;
        std::cout << "--- Phase 2: Server Decodes REQUEST ---" << std::endl;
        std::cout << std::endl;

        auto received_req = MsgPacket::decode(request.wire_data(), request.wire_size());

        std::cout << "  ✓ CRC verified" << std::endl;
        std::cout << "  Received: func=[" << received_req.func()
                  << "] type=[" << (char)received_req.type() << "]" << std::endl;
        std::cout << std::endl;

        std::cout << "  Processing request symbols:" << std::endl;
        while (received_req.fetch_next()) {
            auto sym = received_req.get_value_str("Symbol");
            std::cout << "    Query: " << sym << std::endl;
        }
        std::cout << std::endl;

        /* ========================================================
         * Phase 3: Server 构建 Answer
         * ======================================================== */
        std::cout << "--- Phase 3: Server Builds ANSWER ---" << std::endl;
        std::cout << std::endl;

        MsgPacket answer(MsgType::ANSWER, "V1.0");
        answer.set_func("getData");
        answer.set_timestamp(nullptr);
        answer.set_headers(4, "Symbol,Price,Volume,Time");

        answer.add_row();
        answer.set_value("Symbol", "BTC/USDT");
        answer.set_value("Price", "65500.00");
        answer.set_value("Volume", 2.5);
        answer.set_value("Time", (int64_t)1717000001000LL);

        answer.add_row();
        answer.set_value("Symbol", "ETH/USDT");
        answer.set_value("Price", "3520.00");
        answer.set_value("Volume", 15.8);
        answer.set_value("Time", (int64_t)1717000001000LL);

        answer.add_row();
        answer.set_value("Symbol", "SOL/USDT");
        answer.set_value("Price", "152.00");
        answer.set_value("Volume", 200.0);
        answer.set_value("Time", (int64_t)1717000001000LL);

        answer.finalize();

        print_header(answer, "ANSWER Packet Header");
        std::cout << std::endl;

        std::cout << "  Answer Body (raw with markers):" << std::endl;
        print_body_raw(answer);

        std::cout << std::endl;
        std::cout << "  Answer Body (table view):" << std::endl;
        {
            auto display = local_decode(answer);
            print_table(display);
        }
        std::cout << std::endl;

        /* ========================================================
         * Phase 4: Client 解码 Answer
         * ======================================================== */
        std::cout << "--- Phase 4: Client Decodes ANSWER ---" << std::endl;
        std::cout << std::endl;

        auto received_ans = MsgPacket::decode(answer.wire_data(), answer.wire_size());

        std::cout << "  ✓ CRC verified" << std::endl;
        std::cout << "  Received: func=[" << received_ans.func()
                  << "] type=[" << (char)received_ans.type() << "]" << std::endl;
        std::cout << std::endl;

        std::cout << "  Answer Data (table view):" << std::endl;
        print_table(received_ans);

        std::cout << std::endl;
        std::cout << "  Answer Data (raw with markers):" << std::endl;
        print_body_raw(received_ans);

        std::cout << std::endl;
        std::cout << "=== C++ Demo completed successfully ===" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
