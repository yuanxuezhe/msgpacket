/**
 * rust_rpc_demo.rs — Rust 并发 RPC 演示
 *
 * 并发发送 msgpacket 请求到 RabbitMQ，接收并匹配应答，计算响应时间。
 * 模仿 Python rpc_client.py 的行为，但用 Rust + lapin 实现。
 *
 * 用法:
 *   cargo run --bin rust_rpc_demo
 *
 * 依赖:
 *   - msgpacket C 动态库 (libmsgpacket.so)
 *   - RabbitMQ 服务 (amqp://192.168.10.2:5672/)
 *   - Python echo_demo.py 服务端（需提前启动）
 */

use futures_lite::StreamExt;
use lapin::{
    options::*,
    types::FieldTable,
    BasicProperties, Channel, Connection, ConnectionProperties,
};
use libloading::{Library, Symbol};
use std::ffi::{c_char, c_void, CString};
use std::os::raw::c_int;
use std::slice;
use std::time::Instant;

// ================================================================
// FFI 函数类型别名
// ================================================================
type FnMsgCreate = unsafe extern "C" fn(u8, *const c_char) -> *mut c_void;
type FnMsgDestroy = unsafe extern "C" fn(*mut c_void);
type FnMsgFinalize = unsafe extern "C" fn(*mut c_void) -> c_int;
type FnMsgData = unsafe extern "C" fn(*mut c_void) -> *const c_void;
type FnMsgSize = unsafe extern "C" fn(*mut c_void) -> usize;
type FnMsgDecode =
    unsafe extern "C" fn(*const c_void, usize, *mut *mut c_void) -> c_int;
type FnSetFunc = unsafe extern "C" fn(*mut c_void, *const c_char) -> c_int;
type FnSetMsgId = unsafe extern "C" fn(*mut c_void, *const c_char) -> c_int;
type FnSetTimestamp = unsafe extern "C" fn(*mut c_void, *const c_char) -> c_int;
type FnSetHeaders = unsafe extern "C" fn(*mut c_void, c_int, *const c_char) -> c_int;
type FnSetValueStr =
    unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char) -> c_int;
type FnAddRow = unsafe extern "C" fn(*mut c_void) -> c_int;
type FnGetFunc = unsafe extern "C" fn(*mut c_void) -> *const c_char;
type FnGetMsgId = unsafe extern "C" fn(*mut c_void) -> *const c_char;
type FnWireToString = unsafe extern "C" fn(*mut c_void) -> *const c_char;

// ================================================================
// 常量
// ================================================================
const RABBITMQ_URL: &str = "amqp://guest:guest@192.168.10.2:5672/%2f";
const EXCHANGE_NAME: &str = "msgpacket.exchange";
const QUEUE_REQ: &str = "EvTrade.Req";
const QUEUE_ANS: &str = "EvTrade.Ans";

const MSG_TYPE_REQUEST: u8 = 0x52;

// ================================================================
// 全局库句柄
// ================================================================
use std::sync::OnceLock;
static G_LIB: OnceLock<Library> = OnceLock::new();

fn with_lib<F, R>(f: F) -> R
where
    F: FnOnce(&Library) -> R,
{
    let lib = G_LIB.get().unwrap();
    f(lib)
}

unsafe fn read_c_str(ptr: *const c_char, max_len: usize) -> String {
    if ptr.is_null() {
        return String::new();
    }
    let bytes = slice::from_raw_parts(ptr as *const u8, max_len);
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(max_len);
    String::from_utf8_lossy(&bytes[..end])
        .trim_end_matches('\0')
        .to_string()
}

unsafe fn read_c_str_dynamic(ptr: *const c_char) -> String {
    if ptr.is_null() {
        return String::new();
    }
    let mut len = 0;
    while *ptr.add(len) != 0 {
        len += 1;
    }
    let bytes = slice::from_raw_parts(ptr as *const u8, len);
    String::from_utf8_lossy(bytes).to_string()
}

// ================================================================
// Packet 封装
// ================================================================
struct Packet {
    ptr: *mut c_void,
}

// 允许跨线程传递（libloading 的 Library 本身是 Send+Sync，c_void 指针只是地址值）
unsafe impl Send for Packet {}
unsafe impl Sync for Packet {}

impl Packet {
    fn new(msg_type: u8, version: &str) -> Result<Self, String> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnMsgCreate> = lib.get(b"msg_create").unwrap();
            let ver = CString::new(version).unwrap();
            let ptr = f(msg_type, ver.as_ptr());
            if ptr.is_null() {
                Err("msg_create returned NULL".into())
            } else {
                Ok(Packet { ptr })
            }
        })
    }

    fn set_func(&self, func: &str) -> Result<(), String> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnSetFunc> = lib.get(b"msg_set_func").unwrap();
            let s = CString::new(func).unwrap();
            let ret = f(self.ptr, s.as_ptr());
            if ret == 0 { Ok(()) } else { Err(format!("set_func failed: {}", ret)) }
        })
    }

    fn set_msg_id(&self, msg_id: &str) -> Result<(), String> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnSetMsgId> = lib.get(b"msg_set_msg_id").unwrap();
            let s = CString::new(msg_id).unwrap();
            let ret = f(self.ptr, s.as_ptr());
            if ret == 0 { Ok(()) } else { Err(format!("set_msg_id failed: {}", ret)) }
        })
    }

    fn set_timestamp(&self, ts: &str) -> Result<(), String> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnSetTimestamp> = lib.get(b"msg_set_timestamp").unwrap();
            let s = CString::new(ts).unwrap();
            let ret = f(self.ptr, s.as_ptr());
            if ret == 0 { Ok(()) } else { Err(format!("set_timestamp failed: {}", ret)) }
        })
    }

    fn set_headers(&self, ncols: i32, headers: &str) -> Result<(), String> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnSetHeaders> = lib.get(b"msg_set_headers").unwrap();
            let s = CString::new(headers).unwrap();
            let ret = f(self.ptr, ncols, s.as_ptr());
            if ret == 0 { Ok(()) } else { Err(format!("set_headers failed: {}", ret)) }
        })
    }

    fn add_row(&self) -> Result<(), String> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnAddRow> = lib.get(b"msg_add_row").unwrap();
            let ret = f(self.ptr);
            if ret == 0 { Ok(()) } else { Err(format!("add_row failed: {}", ret)) }
        })
    }

    fn set_value(&self, key: &str, value: &str) -> Result<(), String> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnSetValueStr> = lib.get(b"msg_set_value_str").unwrap();
            let k = CString::new(key).unwrap();
            let v = CString::new(value).unwrap();
            let ret = f(self.ptr, k.as_ptr(), v.as_ptr());
            if ret == 0 { Ok(()) } else { Err(format!("set_value failed: {}", ret)) }
        })
    }

    fn finalize(&self) -> Result<(), String> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnMsgFinalize> = lib.get(b"msg_finalize").unwrap();
            let ret = f(self.ptr);
            if ret == 0 { Ok(()) } else { Err(format!("finalize failed: {}", ret)) }
        })
    }

    fn wire_data(&self) -> Vec<u8> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnMsgData> = lib.get(b"msg_data").unwrap();
            let f_size: Symbol<FnMsgSize> = lib.get(b"msg_size").unwrap();
            let ptr = f(self.ptr) as *const u8;
            let size = f_size(self.ptr);
            if ptr.is_null() || size == 0 {
                vec![]
            } else {
                slice::from_raw_parts(ptr, size).to_vec()
            }
        })
    }

    fn func(&self) -> String {
        with_lib(|lib| unsafe {
            let f: Symbol<FnGetFunc> = lib.get(b"msg_get_func").unwrap();
            read_c_str(f(self.ptr), 8).trim().to_string()
        })
    }

    fn msg_id(&self) -> String {
        with_lib(|lib| unsafe {
            let f: Symbol<FnGetMsgId> = lib.get(b"msg_get_msg_id").unwrap();
            read_c_str(f(self.ptr), 32).trim().to_string()
        })
    }

    fn wire_to_string(&self) -> String {
        with_lib(|lib| unsafe {
            let f: Symbol<FnWireToString> = lib.get(b"msg_wire_to_string").unwrap();
            let p = f(self.ptr);
            if p.is_null() { String::new() } else { read_c_str_dynamic(p) }
        })
    }

    fn decode(data: &[u8]) -> Result<Self, String> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnMsgDecode> = lib.get(b"msg_decode").unwrap();
            let mut out: *mut c_void = std::ptr::null_mut();
            let ret = f(data.as_ptr() as *const c_void, data.len(), &mut out);
            if ret != 0 {
                Err(format!("msg_decode failed: {}", ret))
            } else if out.is_null() {
                Err("msg_decode returned NULL".into())
            } else {
                Ok(Packet { ptr: out })
            }
        })
    }
}

impl Drop for Packet {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            with_lib(|lib| unsafe {
                let f: Symbol<FnMsgDestroy> = lib.get(b"msg_destroy").unwrap();
                f(self.ptr)
            });
        }
    }
}

// ================================================================
// 加载 msgpacket 动态库
// ================================================================
fn load_msgpacket_lib() -> Result<(), Box<dyn std::error::Error>> {
    let candidates = vec![
        // Linux
        "../../../library/bin/Lnx64/libmsgpacket.so",
        "../../library/bin/Lnx64/libmsgpacket.so",
        "library/bin/Lnx64/libmsgpacket.so",
        "libmsgpacket.so",
        // Windows
        "../../../library/bin/x64/libmsgpacket.dll",
        "library/bin/x64/libmsgpacket.dll",
        "libmsgpacket.dll",
    ];

    for path in &candidates {
        match unsafe { Library::new(path) } {
            Ok(lib) => {
                eprintln!("  [INFO] Loaded: {}", path);
                G_LIB.set(lib).ok();
                return Ok(());
            }
            Err(_) => continue,
        }
    }
    Err("Cannot find libmsgpacket".into())
}

// ================================================================
// 生成唯一 msg_id
// ================================================================
fn generate_msg_id() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    format!("{:032x}", nanos)
}

// ================================================================
// 生成时间戳字符串 (yyyyMMddHHmmssSSS)
// ================================================================
fn generate_timestamp() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let dur = SystemTime::now().duration_since(UNIX_EPOCH).unwrap();
    let secs = dur.as_secs();
    let millis = dur.subsec_millis();

    let secs_since_2020 = secs.saturating_sub(1577836800);
    let days = secs_since_2020 / 86400;
    let rem = secs_since_2020 % 86400;
    let hour = rem / 3600;
    let minute = (rem % 3600) / 60;
    let second = rem % 60;

    let mut year = 2020i64;
    let mut remaining = days as i64;
    while remaining > 365 {
        let leap = if year % 4 == 0 && (year % 100 != 0 || year % 400 == 0) { 366 } else { 365 };
        if remaining <= leap { break; }
        remaining -= leap;
        year += 1;
    }

    let is_leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    let month_days = if is_leap {
        [31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    } else {
        [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
    };
    let mut month = 1;
    let mut day = remaining as i64 + 1;
    for (i, &md) in month_days.iter().enumerate() {
        if day <= md as i64 {
            break;
        }
        day -= md as i64;
        month = i as i64 + 2;
    }

    format!(
        "{:04}{:02}{:02}{:02}{:02}{:02}{:03}",
        year, month, day, hour, minute, second, millis
    )
}

// ================================================================
// 同步构建 RPC 请求包（返回 wire_data + reply_to）
// ================================================================
fn build_rpc_wire(msg_id: &str, func: &str, params: &[(&str, &str)]) -> Result<(Vec<u8>, String), String> {
    let ts = generate_timestamp();

    let pkt = Packet::new(MSG_TYPE_REQUEST, "V1.0")?;
    pkt.set_msg_id(msg_id)?;
    pkt.set_timestamp(&ts)?;
    pkt.set_func(func)?;

    if !params.is_empty() {
        let headers: Vec<&str> = params.iter().map(|(k, _)| *k).collect();
        pkt.set_headers(params.len() as i32, &headers.join(","))?;
        pkt.add_row()?;
        for (k, v) in params {
            pkt.set_value(k, v)?;
        }
    }

    pkt.finalize()?;
    let wire_data = pkt.wire_data();
    let reply_to = format!("rpc.reply.{}", msg_id);

    Ok((wire_data, reply_to))
}

// ================================================================
// 发送 + 等待应答（统一队列 + correlation_id 匹配 + 5min 超时）
// ================================================================
const RPC_TIMEOUT_SECS: u64 = 300; // 5 分钟超时

async fn call_rpc(
    channel: &Channel,
    ans_consumer: &mut (impl futures_lite::Stream<Item = Result<lapin::message::Delivery, lapin::Error>> + Unpin),
    msg_id: &str,
    func: &str,
    params: &[(&str, &str)],
    reply_to: &str,
) -> Result<(String, i64, Vec<u8>), String> {
    let req_start = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH).unwrap()
        .as_millis() as i64;

    let (wire_data, _) = build_rpc_wire(msg_id, func, params)?;

    println!(
        "  [SEND] msg_id={}, func={}, params={:?}, size={}, reply_to={}",
        msg_id, func, params, wire_data.len(), reply_to
    );

    // 发布消息到 EvTrade.Req，带 reply_to + correlation_id
    // reply_to 队列已在 test_single 中声明
    let properties = BasicProperties::default()
        .with_reply_to(reply_to.into())
        .with_correlation_id(msg_id.into());

    channel
        .basic_publish(
            EXCHANGE_NAME,
            QUEUE_REQ,
            BasicPublishOptions::default(),
            &wire_data,
            properties,
        )
        .await
        .map_err(|e| format!("basic_publish failed: {}", e))?;

    // 等待应答（从 reply_to 队列，5min 超时）
    // 注意：Python echo 服务端反射请求时不带 correlation_id，
    // 所以用私有 reply_to 队列时直接取第一个 delivery 即可（队列只有预期应答）
    let reply_data = tokio::time::timeout(
        std::time::Duration::from_secs(RPC_TIMEOUT_SECS),
        async {
            match ans_consumer.next().await {
                Some(Ok(delivery)) => {
                    delivery.ack(BasicAckOptions::default()).await.ok();
                    Ok(delivery.data)
                }
                Some(Err(e)) => Err(format!("consumer error: {}", e)),
                None => Err("应答队列关闭".into()),
            }
        },
    )
    .await
    .map_err(|_| "RPC timeout (5min)".to_string())?
    .map_err(|e| e)?;

    let recv_time = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH).unwrap()
        .as_millis() as i64;

    Ok((reply_to.to_string(), recv_time - req_start, reply_data))
}

// ================================================================
// 单请求测试（共享 EvTrade.Ans 队列 + correlation_id 匹配）
// ================================================================
async fn test_single(channel: &Channel) -> Result<(), String> {
    println!("\n=== 单请求测试 ===\n");

    // 创建共享的应答消费者（从私有 reply_to 队列）
    let reply_to = format!("rpc.reply.{}", generate_msg_id());

    // 先声明队列（确保存在）
    channel
        .queue_declare(
            &reply_to,
            QueueDeclareOptions {
                durable: false,
                exclusive: true,
                auto_delete: true,
                nowait: false,
                passive: false,
            },
            FieldTable::default(),
        )
        .await
        .map_err(|e| format!("queue_declare failed: {}", e))?;

    // 在 reply_to 队列上创建消费者
    let mut ans_consumer = channel
        .basic_consume(
            &reply_to,
            "ans_consumer",
            BasicConsumeOptions::default(),
            FieldTable::default(),
        )
        .await
        .map_err(|e| format!("basic_consume failed: {}", e))?;

    let msg_id = generate_msg_id();
    let (_, latency, resp_data) = call_rpc(channel, &mut ans_consumer, &msg_id, "echo", &[
        ("key", "single_test"),
        ("value", "12345"),
    ], &reply_to)
    .await?;

    if let Ok(resp_pkt) = Packet::decode(&resp_data) {
        println!(
            "  [RECV] msg_id={}, func={}, latency={}ms, size={}",
            resp_pkt.msg_id(),
            resp_pkt.func(),
            latency,
            resp_data.len()
        );
        println!("  [BODY] {}", resp_pkt.wire_to_string());
    } else {
        println!("  [RECV] decode failed, latency={}ms", latency);
    }

    Ok(())
}

// ================================================================
// 并发测试：使用 tokio::spawn 实现真正并发
// 注意：这里仍用私有 reply_to 队列，因为并发场景下共享 EvTrade.Ans
// 需要每个请求有独立的消费者来避免匹配混乱，这里用独立 consumer 模式
// ================================================================
async fn test_concurrent(channel: &Channel, num: usize) -> Result<(), String> {
    println!("\n=== 并发测试 ({} 个请求) ===\n", num);

    // 测试用例
    let test_cases: [(&str, Vec<(&str, &str)>); 8] = [
        ("echo", vec![("key", "value")]),
        ("echo", vec![("msg", "hello rust")]),
        ("echo", vec![("data", "test123")]),
        ("getData", vec![]),
        ("calc", vec![("a", "100"), ("b", "200")]),
        ("calc", vec![("a", "3.14"), ("b", "2.718")]),
        ("echo", vec![("seq", "0")]),
        ("echo", vec![("seq", "1")]),
    ];

    let start_time = Instant::now();

    // 克隆 channel 供并发使用
    let channel = channel.clone();

    // 创建独立的私有 reply_to 消费者（每个并发请求独立）
    async fn call_rpc_concurrent(
        channel: &Channel,
        msg_id: &str,
        func: &str,
        params: &[(&str, &str)],
        consumer_id: usize,
    ) -> Result<(String, i64, Vec<u8>), String> {
        let req_start = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH).unwrap()
            .as_millis() as i64;

        let (wire_data, reply_to) = build_rpc_wire(msg_id, func, params)?;

        // 声明私有 reply queue（独立队列，避免并发冲突）
        channel
            .queue_declare(
                &reply_to,
                QueueDeclareOptions {
                    durable: false,
                    exclusive: true,
                    auto_delete: true,
                    nowait: false,
                    passive: false,
                },
                FieldTable::default(),
            )
            .await
            .map_err(|e| format!("queue_declare failed: {}", e))?;

        // 发布消息（带 reply_to + correlation_id）
        let properties = BasicProperties::default()
            .with_reply_to(reply_to.clone().into())
            .with_correlation_id(msg_id.into());

        channel
            .basic_publish(
                EXCHANGE_NAME,
                QUEUE_REQ,
                BasicPublishOptions::default(),
                &wire_data,
                properties,
            )
            .await
            .map_err(|e| format!("basic_publish failed: {}", e))?;

        // 等待应答（5min 超时）
        let reply_data = tokio::time::timeout(
            std::time::Duration::from_secs(RPC_TIMEOUT_SECS),
            async {
                // 每个并发请求使用独立的 consumer tag（避免冲突）
                let consumer_tag = format!("rpc_consumer_{}_{}", &msg_id[..8], consumer_id);
                let mut consumer = channel
                    .basic_consume(
                        &reply_to,
                        &consumer_tag,
                        BasicConsumeOptions::default(),
                        FieldTable::default(),
                    )
                    .await
                    .map_err(|e| format!("basic_consume failed: {}", e))?;

                while let Some(delivery_result) = consumer.next().await {
                    match delivery_result {
                        Ok(delivery) => {
                            delivery.ack(BasicAckOptions::default()).await.ok();
                            return Ok(delivery.data);
                        }
                        Err(e) => return Err(format!("consumer error: {}", e)),
                    }
                }
                Err("consumer closed".into())
            },
        )
        .await
        .map_err(|_| "RPC timeout (5min)".to_string())?
        .map_err(|e| e)?;

        let recv_time = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH).unwrap()
            .as_millis() as i64;

        Ok((reply_to, recv_time - req_start, reply_data))
    }

    let mut handles = Vec::new();

    for i in 0..num {
        let ch = channel.clone();
        let (func_name, params): (String, Vec<(String, String)>) = {
            let tc = &test_cases[i % test_cases.len()];
            (tc.0.to_string(), tc.1.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect())
        };

        let handle = tokio::spawn(async move {
            let msg_id = generate_msg_id();
            let params_ref: Vec<(&str, &str)> = params.iter().map(|(k, v)| (k.as_str(), v.as_str())).collect();
            let result: Result<(String, i64, Vec<u8>), String> = call_rpc_concurrent(&ch, &msg_id, &func_name, &params_ref, i).await;
            (i, msg_id, result)
        });

        handles.push(handle);
    }

    // 收集结果
    let mut results: Vec<(usize, String, Result<(String, i64, Vec<u8>), String>)> = Vec::new();
    for handle in handles {
        if let Ok(result) = handle.await {
            results.push(result);
        }
    }

    // 按顺序打印结果
    results.sort_by_key(|(i, _, _)| *i);
    for (i, _msg_id, result) in results {
        match result {
            Ok((reply_to, latency, resp_data)) => {
                if let Ok(resp_pkt) = Packet::decode(&resp_data) {
                    println!(
                        "  [RECV #{}] msg_id={}, func={}, latency={}ms, size={}",
                        i + 1, resp_pkt.msg_id(), resp_pkt.func(), latency, resp_data.len()
                    );
                } else {
                    println!("  [RECV #{}] reply_to={}, latency={}ms, decode failed", i + 1, reply_to, latency);
                }
            }
            Err(e) => {
                println!("  [ERROR #{}] {}", i + 1, e);
            }
        }
    }

    let elapsed = start_time.elapsed();
    println!("\n  === 完成 {} 个并发请求，耗时 {:?} ===", num, elapsed);

    Ok(())
}

// ================================================================
// 主函数
// ================================================================
#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("=== Rust 并发 RPC Demo ===\n");

    // 加载 msgpacket 动态库
    print!("Loading msgpacket library... ");
    load_msgpacket_lib()?;
    println!("OK\n");

    // 连接 RabbitMQ
    print!("Connecting to RabbitMQ... ");
    let conn = Connection::connect(RABBITMQ_URL, ConnectionProperties::default())
        .await
        .map_err(|e| format!("connect failed: {}", e))?;
    println!("OK");

    let channel = conn.create_channel().await
        .map_err(|e| format!("create channel failed: {}", e))?;

    // 设置 QoS
    channel
        .basic_qos(10, BasicQosOptions::default())
        .await
        .map_err(|e| format!("basic_qos failed: {}", e))?;

    // 声明 exchange
    channel
        .exchange_declare(
            EXCHANGE_NAME,
            lapin::ExchangeKind::Topic,
            ExchangeDeclareOptions {
                durable: true,
                nowait: false,
                passive: false,
                internal: false,
                auto_delete: false,
            },
            FieldTable::default(),
        )
        .await
        .map_err(|e| format!("exchange_declare failed: {}", e))?;

    // 声明请求队列并绑定
    channel
        .queue_declare(
            QUEUE_REQ,
            QueueDeclareOptions {
                durable: true,
                exclusive: false,
                auto_delete: false,
                nowait: false,
                passive: false,
            },
            FieldTable::default(),
        )
        .await
        .map_err(|e| format!("queue_declare failed: {}", e))?;

    channel
        .queue_bind(
            QUEUE_REQ,
            EXCHANGE_NAME,
            QUEUE_REQ,
            QueueBindOptions::default(),
            FieldTable::default(),
        )
        .await
        .map_err(|e| format!("queue_bind failed: {}", e))?;

    // 声明应答队列并绑定
    channel
        .queue_declare(
            QUEUE_ANS,
            QueueDeclareOptions {
                durable: true,
                exclusive: false,
                auto_delete: false,
                nowait: false,
                passive: false,
            },
            FieldTable::default(),
        )
        .await
        .map_err(|e| format!("queue_declare failed: {}", e))?;

    channel
        .queue_bind(
            QUEUE_ANS,
            EXCHANGE_NAME,
            QUEUE_ANS,
            QueueBindOptions::default(),
            FieldTable::default(),
        )
        .await
        .map_err(|e| format!("queue_bind failed: {}", e))?;

    println!("Connected: {}", RABBITMQ_URL);
    println!("Exchange: {}, ReqQueue: {}, AnsQueue: {}\n", EXCHANGE_NAME, QUEUE_REQ, QUEUE_ANS);

    // 单请求测试
    test_single(&channel).await?;

    // 并发测试
    test_concurrent(&channel, 8).await?;

    println!("\n=== Demo 完成 ===");
    Ok(())
}
