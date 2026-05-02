/**
 * main.rs — MsgPacket Rust FFI Demo
 *
 * Uses libloading for runtime dynamic loading of the C DLL.
 * Demonstrates complete Request-Answer pack/unpack cycle
 * with human-readable output.
 */

use libloading::{Library, Symbol};
use std::ffi::{c_char, c_void, CString};
use std::os::raw::c_int;

// ================================================================
// Constants
// ================================================================
const MSG_TYPE_REQUEST:   u8 = 0x52; // 'R'
const MSG_TYPE_ANSWER:    u8 = 0x41; // 'A'
const MSG_TYPE_PUSH:      u8 = 0x50; // 'P'
const MSG_TYPE_HEARTBEAT: u8 = 0x48; // 'H'
const MSG_CODE_SUCCESS:   &str = "00001";
const HEAD_SIZE:          usize = 72;
const BODY_OFFSET:        usize = 4 + 4 + 4 + HEAD_SIZE; // magic + crc32 + body_len + header = 84

// ================================================================
// FFI Function Types
// ================================================================
type FnMsgCreate        = unsafe extern "C" fn(u8, *const c_char) -> *mut c_void;
type FnMsgDestroy       = unsafe extern "C" fn(*mut c_void);
type FnMsgFinalize      = unsafe extern "C" fn(*mut c_void) -> c_int;
type FnMsgData          = unsafe extern "C" fn(*mut c_void) -> *const c_void;
type FnMsgSize          = unsafe extern "C" fn(*mut c_void) -> usize;
type FnMsgDecode        = unsafe extern "C" fn(*const c_void, usize, *mut *mut c_void) -> c_int;
type FnMsgEncode        = unsafe extern "C" fn(*mut c_void, *mut *mut c_void, *mut usize) -> c_int;
type FnMsgFreeBuffer    = unsafe extern "C" fn(*mut c_void);

type FnSetFunc          = unsafe extern "C" fn(*mut c_void, *const c_char) -> c_int;
type FnSetCode          = unsafe extern "C" fn(*mut c_void, *const c_char) -> c_int;
type FnSetTimestamp     = unsafe extern "C" fn(*mut c_void, *const c_char) -> c_int;
type FnSetHeaders       = unsafe extern "C" fn(*mut c_void, c_int, *const c_char) -> c_int;

type FnGetMsgId         = unsafe extern "C" fn(*mut c_void) -> *const c_char;
type FnGetFunc          = unsafe extern "C" fn(*mut c_void) -> *const c_char;
type FnGetVersion       = unsafe extern "C" fn(*mut c_void) -> *const c_char;
type FnGetType          = unsafe extern "C" fn(*mut c_void) -> u8;
type FnGetCode          = unsafe extern "C" fn(*mut c_void) -> *const c_char;
type FnGetTimestamp     = unsafe extern "C" fn(*mut c_void) -> *const c_char;
type FnGetFormat        = unsafe extern "C" fn(*mut c_void) -> u8;
type FnGetBodyLen       = unsafe extern "C" fn(*mut c_void) -> u32;
type FnGetTotalLen      = unsafe extern "C" fn(*mut c_void) -> usize;
type FnGetHeaderCount   = unsafe extern "C" fn(*mut c_void) -> usize;
type FnGetRowCount      = unsafe extern "C" fn(*mut c_void) -> usize;
type FnGetHeaders       = unsafe extern "C" fn(*mut c_void, *mut c_char, *mut usize) -> c_int;

type FnBeginRow         = unsafe extern "C" fn(*mut c_void) -> c_int;
type FnSetValueStr      = unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char) -> c_int;
type FnSetValueI32      = unsafe extern "C" fn(*mut c_void, *const c_char, i32) -> c_int;
type FnSetValueI64      = unsafe extern "C" fn(*mut c_void, *const c_char, i64) -> c_int;
type FnSetValueDouble   = unsafe extern "C" fn(*mut c_void, *const c_char, f64) -> c_int;

type FnFetchNext        = unsafe extern "C" fn(*mut c_void) -> bool;
type FnResetCursor      = unsafe extern "C" fn(*mut c_void);
type FnGetCurrentRow    = unsafe extern "C" fn(*mut c_void) -> usize;
type FnGetValueStr      = unsafe extern "C" fn(*mut c_void, *const c_char, *mut *const c_char, *mut usize) -> c_int;
type FnGetField         = unsafe extern "C" fn(*mut c_void, usize, usize, *mut *const c_char, *mut usize) -> c_int;

// ================================================================
// Global Library Handle
// ================================================================
static mut G_LIB: Option<Library> = None;

fn load_lib() -> Result<Library, Box<dyn std::error::Error>> {
    let candidates = vec![
        "../../library/bin/x64/libmsgpacket.dll",
        "../library/bin/x64/libmsgpacket.dll",
        "library/bin/x64/libmsgpacket.dll",
        "libmsgpacket.dll",
    ];
    for path in &candidates {
        if let Ok(lib) = unsafe { Library::new(path) } {
            eprintln!("  [INFO] Loaded DLL: {}", path);
            return Ok(lib);
        }
    }
    Err("Cannot find libmsgpacket.dll".into())
}

fn msg_create(msg_type: u8, version: &str) -> *mut c_void {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnMsgCreate> = unsafe { lib.get(b"msg_create").unwrap() };
    let ver = CString::new(version).unwrap();
    unsafe { f(msg_type, ver.as_ptr()) }
}

fn msg_destroy(ptr: *mut c_void) {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnMsgDestroy> = unsafe { lib.get(b"msg_destroy").unwrap() };
    unsafe { f(ptr) }
}

fn msg_set_func(ptr: *mut c_void, func: &str) {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnSetFunc> = unsafe { lib.get(b"msg_set_func").unwrap() };
    let s = CString::new(func).unwrap();
    unsafe { f(ptr, s.as_ptr()) };
}

fn msg_set_code(ptr: *mut c_void, code: &str) {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnSetCode> = unsafe { lib.get(b"msg_set_code").unwrap() };
    let s = CString::new(code).unwrap();
    unsafe { f(ptr, s.as_ptr()) };
}

fn msg_set_timestamp(ptr: *mut c_void, ts: Option<&str>) {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnSetTimestamp> = unsafe { lib.get(b"msg_set_timestamp").unwrap() };
    match ts {
        Some(s) => { let cs = CString::new(s).unwrap(); unsafe { f(ptr, cs.as_ptr()) }; }
        None => { unsafe { f(ptr, std::ptr::null()) }; }
    }
}

fn msg_set_headers(ptr: *mut c_void, ncols: i32, headers: &str) {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnSetHeaders> = unsafe { lib.get(b"msg_set_headers").unwrap() };
    let s = CString::new(headers).unwrap();
    unsafe { f(ptr, ncols, s.as_ptr()) };
}

fn msg_get_msg_id(ptr: *mut c_void) -> String {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnGetMsgId> = unsafe { lib.get(b"msg_get_msg_id").unwrap() };
    let p = unsafe { f(ptr) };
    if p.is_null() { return String::new(); }
    unsafe { read_fixed_str(p, 32) }
}

fn msg_get_func(ptr: *mut c_void) -> String {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnGetFunc> = unsafe { lib.get(b"msg_get_func").unwrap() };
    let p = unsafe { f(ptr) };
    if p.is_null() { return String::new(); }
    unsafe { read_fixed_str(p, 8) }
}

fn msg_get_version(ptr: *mut c_void) -> String {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnGetVersion> = unsafe { lib.get(b"msg_get_version").unwrap() };
    let p = unsafe { f(ptr) };
    if p.is_null() { return String::new(); }
    unsafe { read_fixed_str(p, 8) }
}

fn msg_get_type(ptr: *mut c_void) -> u8 {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnGetType> = unsafe { lib.get(b"msg_get_type").unwrap() };
    unsafe { f(ptr) }
}

fn msg_get_code(ptr: *mut c_void) -> String {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnGetCode> = unsafe { lib.get(b"msg_get_code").unwrap() };
    let p = unsafe { f(ptr) };
    if p.is_null() { return String::new(); }
    unsafe { read_fixed_str(p, 5) }
}

fn msg_get_timestamp(ptr: *mut c_void) -> String {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnGetTimestamp> = unsafe { lib.get(b"msg_get_timestamp").unwrap() };
    let p = unsafe { f(ptr) };
    if p.is_null() { return String::new(); }
    unsafe { read_fixed_str(p, 17) }
}

fn msg_get_format(ptr: *mut c_void) -> u8 {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnGetFormat> = unsafe { lib.get(b"msg_get_format").unwrap() };
    unsafe { f(ptr) }
}

fn msg_get_body_len(ptr: *mut c_void) -> u32 {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnGetBodyLen> = unsafe { lib.get(b"msg_get_body_len").unwrap() };
    unsafe { f(ptr) }
}

fn msg_get_total_len(ptr: *mut c_void) -> usize {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnGetTotalLen> = unsafe { lib.get(b"msg_get_total_len").unwrap() };
    unsafe { f(ptr) }
}

fn msg_get_header_count(ptr: *mut c_void) -> usize {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnGetHeaderCount> = unsafe { lib.get(b"msg_get_header_count").unwrap() };
    unsafe { f(ptr) }
}

fn msg_get_row_count(ptr: *mut c_void) -> usize {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnGetRowCount> = unsafe { lib.get(b"msg_get_row_count").unwrap() };
    unsafe { f(ptr) }
}

fn msg_get_headers(ptr: *mut c_void) -> String {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnGetHeaders> = unsafe { lib.get(b"msg_get_headers").unwrap() };
    let mut buf = vec![0u8; 4096];
    let mut len: usize = 4096;
    unsafe { f(ptr, buf.as_mut_ptr() as *mut c_char, &mut len) };
    // len is now the actual length
    String::from_utf8_lossy(&buf[..len.min(4096)])
        .trim_end_matches('\0')
        .to_string()
}

fn msg_begin_row(ptr: *mut c_void) {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnBeginRow> = unsafe { lib.get(b"msg_begin_row").unwrap() };
    unsafe { f(ptr) };
}

fn msg_set_value_str(ptr: *mut c_void, key: &str, value: &str) {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnSetValueStr> = unsafe { lib.get(b"msg_set_value_str").unwrap() };
    let k = CString::new(key).unwrap();
    let v = CString::new(value).unwrap();
    unsafe { f(ptr, k.as_ptr(), v.as_ptr()) };
}

fn msg_set_value_i64(ptr: *mut c_void, key: &str, value: i64) {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnSetValueI64> = unsafe { lib.get(b"msg_set_value_i64").unwrap() };
    let k = CString::new(key).unwrap();
    unsafe { f(ptr, k.as_ptr(), value) };
}

fn msg_set_value_double(ptr: *mut c_void, key: &str, value: f64) {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnSetValueDouble> = unsafe { lib.get(b"msg_set_value_double").unwrap() };
    let k = CString::new(key).unwrap();
    unsafe { f(ptr, k.as_ptr(), value) };
}

fn msg_finalize(ptr: *mut c_void) -> i32 {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnMsgFinalize> = unsafe { lib.get(b"msg_finalize").unwrap() };
    unsafe { f(ptr) }
}

fn msg_data(ptr: *mut c_void) -> *const u8 {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnMsgData> = unsafe { lib.get(b"msg_data").unwrap() };
    unsafe { f(ptr) as *const u8 }
}

fn msg_size(ptr: *mut c_void) -> usize {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnMsgSize> = unsafe { lib.get(b"msg_size").unwrap() };
    unsafe { f(ptr) }
}

fn msg_decode(data: &[u8]) -> *mut c_void {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnMsgDecode> = unsafe { lib.get(b"msg_decode").unwrap() };
    let mut out: *mut c_void = std::ptr::null_mut();
    let ret = unsafe { f(data.as_ptr() as *const c_void, data.len(), &mut out) };
    if ret != 0 {
        panic!("msg_decode failed: {}", ret);
    }
    out
}

fn msg_encode(ptr: *mut c_void) -> Vec<u8> {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnMsgEncode> = unsafe { lib.get(b"msg_encode").unwrap() };
    let mut out_buf: *mut c_void = std::ptr::null_mut();
    let mut out_len: usize = 0;
    unsafe {
        f(ptr, &mut out_buf, &mut out_len);
        let data = std::slice::from_raw_parts(out_buf as *const u8, out_len).to_vec();
        // free buffer
        let free_fn: Symbol<FnMsgFreeBuffer> = lib.get(b"msg_free_buffer").unwrap();
        free_fn(out_buf);
        data
    }
}

fn msg_fetch_next(ptr: *mut c_void) -> bool {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnFetchNext> = unsafe { lib.get(b"msg_fetch_next").unwrap() };
    unsafe { f(ptr) }
}

fn msg_reset_cursor(ptr: *mut c_void) {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnResetCursor> = unsafe { lib.get(b"msg_reset_cursor").unwrap() };
    unsafe { f(ptr) }
}

fn msg_get_current_row(ptr: *mut c_void) -> usize {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnGetCurrentRow> = unsafe { lib.get(b"msg_get_current_row").unwrap() };
    unsafe { f(ptr) }
}

fn msg_get_value_str(ptr: *mut c_void, key: &str) -> String {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnGetValueStr> = unsafe { lib.get(b"msg_get_value_str").unwrap() };
    let k = CString::new(key).unwrap();
    let mut val: *const c_char = std::ptr::null();
    let mut val_len: usize = 0;
    unsafe {
        f(ptr, k.as_ptr(), &mut val, &mut val_len);
        if val.is_null() || val_len == 0 { return String::new(); }
        read_fixed_str(val, val_len)
    }
}

fn msg_get_field(ptr: *mut c_void, row: usize, col: usize) -> String {
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    let f: Symbol<FnGetField> = unsafe { lib.get(b"msg_get_field").unwrap() };
    let mut val: *const c_char = std::ptr::null();
    let mut val_len: usize = 0;
    unsafe {
        f(ptr, row, col, &mut val, &mut val_len);
        if val.is_null() || val_len == 0 { return String::new(); }
        read_fixed_str(val, val_len)
    }
}

unsafe fn read_fixed_str(ptr: *const c_char, max_len: usize) -> String {
    let bytes = unsafe { std::slice::from_raw_parts(ptr as *const u8, max_len) };
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(max_len);
    String::from_utf8_lossy(&bytes[..end]).to_string()
}

fn msg_type_name(t: u8) -> &'static str {
    match t {
        MSG_TYPE_REQUEST   => "REQUEST",
        MSG_TYPE_ANSWER    => "ANSWER",
        MSG_TYPE_PUSH      => "PUSH",
        MSG_TYPE_HEARTBEAT => "HEARTBEAT",
        _ => "UNKNOWN",
    }
}

// ================================================================
// Safe Wrapper Struct
// ================================================================
struct Packet {
    ptr: *mut c_void,
}

impl Packet {
    fn new(msg_type: u8, version: &str) -> Self {
        let ptr = msg_create(msg_type, version);
        if ptr.is_null() {
            panic!("msg_create failed");
        }
        Packet { ptr }
    }

    fn decode(data: &[u8]) -> Self {
        let ptr = msg_decode(data);
        Packet { ptr }
    }

    fn set_func(&self, f: &str) { msg_set_func(self.ptr, f); }
    fn set_code(&self, c: &str) { msg_set_code(self.ptr, c); }
    fn set_timestamp(&self, ts: Option<&str>) { msg_set_timestamp(self.ptr, ts); }
    fn set_headers(&self, n: i32, h: &str) { msg_set_headers(self.ptr, n, h); }

    fn msg_id(&self) -> String { msg_get_msg_id(self.ptr) }
    fn func(&self) -> String { msg_get_func(self.ptr) }
    fn version(&self) -> String { msg_get_version(self.ptr) }
    fn msg_type(&self) -> u8 { msg_get_type(self.ptr) }
    fn code(&self) -> String { msg_get_code(self.ptr) }
    fn timestamp(&self) -> String { msg_get_timestamp(self.ptr) }
    fn format(&self) -> u8 { msg_get_format(self.ptr) }
    fn body_len(&self) -> u32 { msg_get_body_len(self.ptr) }
    fn total_len(&self) -> usize { msg_get_total_len(self.ptr) }
    fn header_count(&self) -> usize { msg_get_header_count(self.ptr) }
    fn row_count(&self) -> usize { msg_get_row_count(self.ptr) }
    fn get_headers(&self) -> String { msg_get_headers(self.ptr) }

    fn begin_row(&self) { msg_begin_row(self.ptr); }
    fn set_value_str(&self, k: &str, v: &str) { msg_set_value_str(self.ptr, k, v); }
    fn set_value_i64(&self, k: &str, v: i64) { msg_set_value_i64(self.ptr, k, v); }
    fn set_value_f64(&self, k: &str, v: f64) { msg_set_value_double(self.ptr, k, v); }

    fn finalize(&self) -> i32 { msg_finalize(self.ptr) }
    fn wire_data(&self) -> &[u8] {
        let ptr = msg_data(self.ptr);
        let size = msg_size(self.ptr);
        if ptr.is_null() || size == 0 { return &[]; }
        unsafe { std::slice::from_raw_parts(ptr, size) }
    }

    fn fetch_next(&self) -> bool { msg_fetch_next(self.ptr) }
    fn reset_cursor(&self) { msg_reset_cursor(self.ptr); }
    fn current_row(&self) -> usize { msg_get_current_row(self.ptr) }
    fn get_value(&self, key: &str) -> String { msg_get_value_str(self.ptr, key) }
    fn get_field_at(&self, row: usize, col: usize) -> String {
        msg_get_field(self.ptr, row, col)
    }
}

impl Drop for Packet {
    fn drop(&mut self) {
        if !self.ptr.is_null() {
            msg_destroy(self.ptr);
        }
    }
}

// ================================================================
// Helpers
// ================================================================
fn print_header(pkt: &Packet) {
    let ts = pkt.timestamp();
    let ts_display = if ts.len() >= 17 {
        format!("{} ({}-{}-{} {}:{}:{}.{})",
                ts,
                &ts[0..4], &ts[4..6], &ts[6..8],
                &ts[8..10], &ts[10..12], &ts[12..14], &ts[14..17])
    } else {
        String::from("N/A")
    };

    println!("  msg_id:    {}", pkt.msg_id());
    println!("  version:   {}", pkt.version());
    println!("  func:      {}", pkt.func());
    println!("  type:      {} ({})",
             msg_type_name(pkt.msg_type()),
             pkt.msg_type() as char);
    println!("  format:    {} '{}'", pkt.format(), pkt.format() as char);
    println!("  code:      {}", pkt.code());
    println!("  timestamp: {}", ts_display);
    println!("  body_len:  {} bytes", pkt.body_len());
    println!("  total_len: {} bytes", pkt.total_len());
}

fn print_body_raw(pkt: &Packet) {
    let data = pkt.wire_data();
    if data.len() == 0 { println!("  (no data)"); return; }
    let start = BODY_OFFSET;
    let end = start + pkt.body_len() as usize;
    if start >= data.len() { return; }
    let body = &data[start..end.min(data.len())];
    print!("  Body raw ({} bytes): ", body.len());
    for &b in body {
        match b {
            0x1F => print!("<US>"),
            0x1E => print!("<RS>"),
            0x1C => print!("<FS>"),
            0x1B => print!("<ESC>"),
            _ if b.is_ascii_graphic() || b == b' ' => print!("{}", b as char),
            _ => print!("#"),
        }
    }
    println!();
}

fn print_table(pkt: &Packet) {
    let hdr_cnt = pkt.header_count();
    if hdr_cnt == 0 { println!("  (no headers)"); return; }

    let hdr_str = pkt.get_headers();
    let header_names: Vec<&str> = hdr_str.split(',').map(|s| s.trim()).collect();
    let mut col_widths: Vec<usize> = header_names.iter().map(|n| n.len()).collect();

    // Scan data for column widths
    pkt.reset_cursor();
    while pkt.fetch_next() {
        for col in 0..hdr_cnt {
            let val = pkt.get_field_at(pkt.current_row(), col);
            if val.len() > col_widths[col] {
                col_widths[col] = val.len();
            }
        }
    }
    pkt.reset_cursor();

    // Print header
    print!("  | ");
    for (i, name) in header_names.iter().enumerate() {
        print!("{:<width$}", name, width = col_widths[i]);
        if i < hdr_cnt - 1 { print!(" | "); }
    }
    println!(" |");

    // Separator
    print!("  |-");
    for (i, w) in col_widths.iter().enumerate() {
        print!("{}", "-".repeat(*w));
        if i < hdr_cnt - 1 { print!("-+-"); }
    }
    println!("-|");

    // Data rows
    while pkt.fetch_next() {
        print!("  | ");
        for col in 0..hdr_cnt {
            let val = pkt.get_field_at(pkt.current_row(), col);
            print!("{:<width$}", val, width = col_widths[col]);
            if col < hdr_cnt - 1 { print!(" | "); }
        }
        println!(" |");
    }
    pkt.reset_cursor();
}

fn local_decode_for_display(pkt: &Packet) -> Packet {
    let encoded = msg_encode(pkt.ptr);
    Packet::decode(&encoded)
}

// ================================================================
// Main
// ================================================================
fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("{}", "=".repeat(60));
    println!("  MsgPacket Rust FFI Demo - Request/Answer Full Cycle");
    println!("{}", "=".repeat(60));

    // Find and load the DLL
    let lib = load_lib()?;
    unsafe { G_LIB = Some(lib) };

    // ==============================================================
    // Phase 1: Client builds REQUEST
    // ==============================================================
    println!("\n--- Phase 1: Client Builds REQUEST ---\n");

    let request = Packet::new(MSG_TYPE_REQUEST, "V1.0");
    request.set_func("getData");
    request.set_code(MSG_CODE_SUCCESS);
    request.set_timestamp(None);
    request.set_headers(4, "Symbol,Price,Volume,Time");

    request.begin_row();
    request.set_value_str("Symbol", "BTC/USDT");
    request.set_value_str("Price", "65000.50");
    request.set_value_f64("Volume", 1.2);
    request.set_value_i64("Time", 1717000000000);

    request.begin_row();
    request.set_value_str("Symbol", "ETH/USDT");
    request.set_value_str("Price", "3500.00");
    request.set_value_f64("Volume", 10.5);
    request.set_value_i64("Time", 1717000000000);

    request.begin_row();
    request.set_value_str("Symbol", "SOL/USDT");
    request.set_value_str("Price", "150.00");
    request.set_value_f64("Volume", 100.0);
    request.set_value_i64("Time", 1717000000000);

    let ret = request.finalize();
    if ret != 0 {
        eprintln!("  [ERROR] finalize failed: {}", ret);
        return Ok(());
    }

    print_header(&request);

    let magic = &request.wire_data()[..4];
    println!("\n  Magic: '{}' [OK]", String::from_utf8_lossy(magic));

    println!("\n  Request Body (raw with markers):");
    print_body_raw(&request);

    println!("\n  Request Body (table view):");
    {
        let display = local_decode_for_display(&request);
        print_table(&display);
    }

    println!("\n  [OK] Built request: {} bytes", request.wire_data().len());

    // ==============================================================
    // Phase 2: Server decodes REQUEST
    // ==============================================================
    println!("\n--- Phase 2: Server Decodes REQUEST ---\n");

    let received_req = Packet::decode(request.wire_data());

    println!("  [OK] CRC verified");
    println!("  Received: func=[{}] type=[{}] code=[{}]",
             received_req.func(),
             received_req.msg_type() as char,
             received_req.code());

    println!("\n  Processing request symbols:");
    while received_req.fetch_next() {
        let sym = received_req.get_value("Symbol");
        println!("    Query: {}", sym);
    }

    // ==============================================================
    // Phase 3: Server builds ANSWER
    // ==============================================================
    println!("\n--- Phase 3: Server Builds ANSWER ---\n");

    let answer = Packet::new(MSG_TYPE_ANSWER, "V1.0");
    answer.set_func("getData");
    answer.set_code(MSG_CODE_SUCCESS);
    answer.set_timestamp(None);
    answer.set_headers(4, "Symbol,Price,Volume,Time");

    answer.begin_row();
    answer.set_value_str("Symbol", "BTC/USDT");
    answer.set_value_str("Price", "65500.00");
    answer.set_value_f64("Volume", 2.5);
    answer.set_value_i64("Time", 1717000001000);

    answer.begin_row();
    answer.set_value_str("Symbol", "ETH/USDT");
    answer.set_value_str("Price", "3520.00");
    answer.set_value_f64("Volume", 15.8);
    answer.set_value_i64("Time", 1717000001000);

    answer.begin_row();
    answer.set_value_str("Symbol", "SOL/USDT");
    answer.set_value_str("Price", "152.00");
    answer.set_value_f64("Volume", 200.0);
    answer.set_value_i64("Time", 1717000001000);

    answer.finalize();

    print_header(&answer);

    println!("\n  Answer Body (raw with markers):");
    print_body_raw(&answer);

    println!("\n  Answer Body (table view):");
    {
        let display = local_decode_for_display(&answer);
        print_table(&display);
    }

    println!("\n  [OK] Built answer: {} bytes", answer.wire_data().len());

    // ==============================================================
    // Phase 4: Client decodes ANSWER
    // ==============================================================
    println!("\n--- Phase 4: Client Decodes ANSWER ---\n");

    let received_ans = Packet::decode(answer.wire_data());

    println!("  [OK] CRC verified");
    println!("  Received: func=[{}] type=[{}] code=[{}]",
             received_ans.func(),
             received_ans.msg_type() as char,
             received_ans.code());

    println!("\n  Answer Data (table view):");
    print_table(&received_ans);

    println!("\n  Answer Data (raw with markers):");
    print_body_raw(&received_ans);

    println!("\n{}", "=".repeat(60));
    println!("  Rust Demo completed successfully!");
    println!("{}", "=".repeat(60));

    Ok(())
}
