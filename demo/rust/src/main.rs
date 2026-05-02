/**
 * main.rs — MsgPacket Rust 多结果集打包示例
 *
 * 使用 msgpacket 模块，模仿 C 语言 demo_builder.c 的写法，
 * 演示如何构建含多个结果集的包，并解码验证。
 */

mod msgpacket;
use msgpacket::{Packet, load_library, MsgError};
use msgpacket::MSG_TYPE_REQUEST;

fn print_result_set_info(pkt: &Packet, rs_number: usize) {
    println!("  Result Set {}:", rs_number);
    println!("    headers: {}, rows: {}", pkt.header_count(), pkt.row_count());

    let hdr_str = pkt.get_headers();
    println!("    header: {}", hdr_str);

    pkt.reset_cursor();
    while pkt.fetch_next() {
        let row = pkt.current_row();
        print!("    row {}:", row);
        for col in 0..pkt.header_count() {
            let val = pkt.get_field(row, col);
            print!(" {}", val);
        }
        println!();
    }
    pkt.reset_cursor();
}

fn decode_and_print_all_rs(wire_data: &[u8]) -> Result<(), MsgError> {
    let pkt = Packet::decode(wire_data)?;

    println!("  === Decoded Packet ===");
    println!("  func: {}  type: {}  code: {}",
             pkt.func(), pkt.msg_type() as char, pkt.code());
    println!("  result_set_count: {}", pkt.result_set_count());

    println!("  === Iterate all result sets ===");
    for rs in 1..=pkt.result_set_count() {
        if rs > 1 {
            if !pkt.next_result_set() {
                println!("  Failed to switch to RS{}", rs);
                break;
            }
        }
        println!("  [RS{}] current_rs={}, rs_count={}, row_count={}",
                 rs, pkt.result_set(), pkt.result_set_count(), pkt.row_count());
        print_result_set_info(&pkt, rs);
    }
    Ok(())
}

fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("=== MsgPacket Multi-Result-Set Rust Demo ===\n");

    // 加载动态库
    load_library()?;

    // ==============================================================
    // 构建含多个结果集的包
    // ==============================================================
    println!("--- Build multi result-set packet ---\n");

    let sender = Packet::new(MSG_TYPE_REQUEST, "V1.0")?;
    sender.set_func("subscribe")?;
    sender.set_code("00001")?;
    sender.set_timestamp(Some("20260501090101123"))?;

    // RS1: 请求参数
    println!("\n  Building RS1...");
    sender.set_headers(2, "Symbol,Price")?;
    sender.add_row()?;
    sender.set_value("Symbol", "BTC/USDT")?;
    sender.set_value("Price", "65000.50")?;

    sender.add_row()?;
    sender.set_value("Symbol", "ETH/USDT")?;
    sender.set_value("Price", "3500.00")?;

    // RS2: 附加信息
    println!("  Adding RS2...");
    if !sender.add_result_set() {
        println!("  msg_add_result_set failed");
        return Ok(());
    }
    sender.set_headers(2, "Tag,Note")?;
    sender.add_row()?;
    sender.set_value("Tag", "priority")?;
    sender.set_value("Note", "high-frequency")?;
    println!("  RS2 row_count: {}", sender.row_count());

    // RS3: 扩展字段
    println!("  Adding RS3...");
    if !sender.add_result_set() {
        println!("  msg_add_result_set failed");
        return Ok(());
    }
    sender.set_headers(2, "Ext1,Ext2")?;
    sender.add_row()?;
    sender.add_row()?;
    sender.add_row()?;
    sender.set_value("Ext1", "ext_value_1")?;
    sender.set_value("Ext2", "ext_value_2")?;

    sender.finalize()?;

    let wire_data = sender.wire_data();
    let wire_size = wire_data.len();

    println!("\n  Wire data ({} bytes):", wire_size);
    println!("  {}", sender.wire_to_string());

    // ==============================================================
    // 解码验证
    // ==============================================================
    println!("\n--- Decode and verify ---");
    decode_and_print_all_rs(wire_data)?;

    println!("\n=== Rust Demo completed successfully ===");
    Ok(())
}