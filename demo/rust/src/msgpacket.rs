/**
 * msgpacket.rs — MsgPacket Rust FFI 封装模块
 *
 * 提供类型安全的 RAII 封装，委托 C API 实现协议逻辑。
 * 支持构建、解码、遍历多结果集数据包。
 */

use libloading::{Library, Symbol};
use std::ffi::{c_char, c_void, CString};
use std::os::raw::c_int;
use std::slice;
use std::str;

// ================================================================
// 常量
// ================================================================
pub const MSG_TYPE_REQUEST:   u8 = 0x52;
pub const MSG_TYPE_ANSWER:    u8 = 0x41;
pub const MSG_TYPE_PUSH:      u8 = 0x50;
pub const MSG_TYPE_HEARTBEAT: u8 = 0x48;

pub const MSG_CODE_SUCCESS: &str = "00001";

pub const HEAD_SIZE:   usize = 72;
pub const BODY_OFFSET: usize = 4 + 4 + 4 + HEAD_SIZE; // 84

// ================================================================
// 错误类型
// ================================================================
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum MsgError {
    NullPtr          = -1,
    InvalidMagic     = -2,
    CrcMismatch      = -3,
    BufferTooSmall   = -4,
    InvalidFormat    = -5,
    InvalidMsgType  = -6,
    EscapeSequence   = -7,
    NoData          = -8,
    BodyTooLarge    = -9,
    TooManyHeaders  = -10,
    TooManyRows     = -11,
    FieldTooLong    = -12,
    VersionMismatch = -13,
    NoMemory        = -14,
    NotFinalized    = -15,
    Unknown         = 0,
}

impl MsgError {
    pub fn from_code(code: i32) -> Self {
        match code {
            -1  => MsgError::NullPtr,
            -2  => MsgError::InvalidMagic,
            -3  => MsgError::CrcMismatch,
            -4  => MsgError::BufferTooSmall,
            -5  => MsgError::InvalidFormat,
            -6  => MsgError::InvalidMsgType,
            -7  => MsgError::EscapeSequence,
            -8  => MsgError::NoData,
            -9  => MsgError::BodyTooLarge,
            -10 => MsgError::TooManyHeaders,
            -11 => MsgError::TooManyRows,
            -12 => MsgError::FieldTooLong,
            -13 => MsgError::VersionMismatch,
            -14 => MsgError::NoMemory,
            -15 => MsgError::NotFinalized,
            _   => MsgError::Unknown,
        }
    }
}

impl std::fmt::Display for MsgError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let s = match self {
            MsgError::NullPtr          => "NullPtr",
            MsgError::InvalidMagic     => "InvalidMagic",
            MsgError::CrcMismatch      => "CrcMismatch",
            MsgError::BufferTooSmall   => "BufferTooSmall",
            MsgError::InvalidFormat    => "InvalidFormat",
            MsgError::InvalidMsgType   => "InvalidMsgType",
            MsgError::EscapeSequence   => "EscapeSequence",
            MsgError::NoData           => "NoData",
            MsgError::BodyTooLarge     => "BodyTooLarge",
            MsgError::TooManyHeaders   => "TooManyHeaders",
            MsgError::TooManyRows      => "TooManyRows",
            MsgError::FieldTooLong      => "FieldTooLong",
            MsgError::VersionMismatch  => "VersionMismatch",
            MsgError::NoMemory          => "NoMemory",
            MsgError::NotFinalized      => "NotFinalized",
            MsgError::Unknown          => "Unknown",
        };
        write!(f, "{}", s)
    }
}

impl std::error::Error for MsgError {}

// ================================================================
// FFI 函数类型别名
// ================================================================
type FnMsgCreate      = unsafe extern "C" fn(u8, *const c_char) -> *mut c_void;
type FnMsgDestroy     = unsafe extern "C" fn(*mut c_void);
type FnMsgFinalize    = unsafe extern "C" fn(*mut c_void) -> c_int;
type FnMsgData        = unsafe extern "C" fn(*mut c_void) -> *const c_void;
type FnMsgSize        = unsafe extern "C" fn(*mut c_void) -> usize;
type FnMsgDecode      = unsafe extern "C" fn(*const c_void, usize, *mut *mut c_void) -> c_int;
type FnSetFunc        = unsafe extern "C" fn(*mut c_void, *const c_char) -> c_int;
type FnSetCode        = unsafe extern "C" fn(*mut c_void, *const c_char) -> c_int;
type FnSetTimestamp   = unsafe extern "C" fn(*mut c_void, *const c_char) -> c_int;
type FnSetHeaders     = unsafe extern "C" fn(*mut c_void, c_int, *const c_char) -> c_int;
type FnGetFunc        = unsafe extern "C" fn(*mut c_void) -> *const c_char;
type FnGetCode        = unsafe extern "C" fn(*mut c_void) -> *const c_char;
type FnGetType        = unsafe extern "C" fn(*mut c_void) -> u8;
type FnGetHeaderCount = unsafe extern "C" fn(*mut c_void) -> usize;
type FnGetRowCount    = unsafe extern "C" fn(*mut c_void) -> usize;
type FnGetHeaders     = unsafe extern "C" fn(*mut c_void, *mut c_char, *mut usize) -> c_int;
type FnAddRow         = unsafe extern "C" fn(*mut c_void) -> c_int;
type FnSetValueStr    = unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char) -> c_int;
type FnFetchNext      = unsafe extern "C" fn(*mut c_void) -> bool;
type FnResetCursor    = unsafe extern "C" fn(*mut c_void);
type FnGetCurrentRow  = unsafe extern "C" fn(*mut c_void) -> usize;
type FnGetValueStr    = unsafe extern "C" fn(*mut c_void, *const c_char, *mut *const c_char, *mut usize) -> c_int;
type FnGetField       = unsafe extern "C" fn(*mut c_void, usize, usize, *mut *const c_char, *mut usize) -> c_int;
type FnGetResultSet       = unsafe extern "C" fn(*mut c_void) -> usize;
type FnAddResultSet       = unsafe extern "C" fn(*mut c_void) -> bool;
type FnNextResultSet      = unsafe extern "C" fn(*mut c_void) -> bool;
type FnGetResultSetCount  = unsafe extern "C" fn(*mut c_void) -> usize;
type FnSelectResultSet    = unsafe extern "C" fn(*mut c_void, usize) -> c_int;
type FnWireToString       = unsafe extern "C" fn(*mut c_void) -> *const c_char;
type FnGetMsgId           = unsafe extern "C" fn(*mut c_void) -> *const c_char;
type FnGetTimestamp       = unsafe extern "C" fn(*mut c_void) -> *const c_char;
type FnGetBodyLen         = unsafe extern "C" fn(*mut c_void) -> u32;
type FnGetTotalLen        = unsafe extern "C" fn(*mut c_void) -> usize;

// ================================================================
// 库加载器
// ================================================================
static mut G_LIB: Option<Library> = None;

fn with_lib<F, R>(f: F) -> R
where
    F: FnOnce(&Library) -> R,
{
    let lib = unsafe { G_LIB.as_ref().unwrap() };
    f(lib)
}

// ================================================================
// 辅助函数
// ================================================================
unsafe fn read_c_str(ptr: *const c_char, max_len: usize) -> String {
    if ptr.is_null() {
        return String::new();
    }
    let bytes = slice::from_raw_parts(ptr as *const u8, max_len);
    let end = bytes.iter().position(|&b| b == 0).unwrap_or(max_len);
    String::from_utf8_lossy(&bytes[..end]).trim_end_matches('\0').to_string()
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
// Packet — 主结构体
// ================================================================
pub struct Packet {
    ptr: *mut c_void,
    owned: bool,
}

impl Packet {
    // --------------------------------------------------------
    // 构造与销毁
    // --------------------------------------------------------

    /// 创建新的数据包
    pub fn new(msg_type: u8, version: &str) -> Result<Self, MsgError> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnMsgCreate> = lib.get(b"msg_create").unwrap();
            let ver = CString::new(version).unwrap();
            let ptr = f(msg_type, ver.as_ptr());
            if ptr.is_null() {
                Err(MsgError::NoMemory)
            } else {
                Ok(Packet { ptr, owned: true })
            }
        })
    }

    /// 从 wire 字节流解码
    pub fn decode(data: &[u8]) -> Result<Self, MsgError> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnMsgDecode> = lib.get(b"msg_decode").unwrap();
            let mut out: *mut c_void = std::ptr::null_mut();
            let ret = f(data.as_ptr() as *const c_void, data.len(), &mut out);
            if ret != 0 {
                Err(MsgError::from_code(ret))
            } else if out.is_null() {
                Err(MsgError::NoMemory)
            } else {
                Ok(Packet { ptr: out, owned: true })
            }
        })
    }

    /// 获取 wire 数据（finalize 后有效）
    pub fn wire_data(&self) -> &[u8] {
        with_lib(|lib| unsafe {
            let f: Symbol<FnMsgData> = lib.get(b"msg_data").unwrap();
            let f_size: Symbol<FnMsgSize> = lib.get(b"msg_size").unwrap();
            let ptr = f(self.ptr) as *const u8;
            let size = f_size(self.ptr);
            if ptr.is_null() || size == 0 {
                &[]
            } else {
                slice::from_raw_parts(ptr, size)
            }
        })
    }

    /// 获取 wire 数据字节长度
    pub fn wire_size(&self) -> usize {
        with_lib(|lib| unsafe {
            let f: Symbol<FnMsgSize> = lib.get(b"msg_size").unwrap();
            f(self.ptr)
        })
    }

    /// 将 wire 数据转为可读字符串
    pub fn wire_to_string(&self) -> String {
        with_lib(|lib| unsafe {
            let f: Symbol<FnWireToString> = lib.get(b"msg_wire_to_string").unwrap();
            let p = f(self.ptr);
            if p.is_null() {
                String::new()
            } else {
                read_c_str_dynamic(p)
            }
        })
    }

    // --------------------------------------------------------
    // Header 设置
    // --------------------------------------------------------

    pub fn set_func(&self, func: &str) -> Result<(), MsgError> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnSetFunc> = lib.get(b"msg_set_func").unwrap();
            let s = CString::new(func).unwrap();
            let ret = f(self.ptr, s.as_ptr());
            if ret == 0 { Ok(()) } else { Err(MsgError::from_code(ret)) }
        })
    }

    pub fn set_code(&self, code: &str) -> Result<(), MsgError> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnSetCode> = lib.get(b"msg_set_code").unwrap();
            let s = CString::new(code).unwrap();
            let ret = f(self.ptr, s.as_ptr());
            if ret == 0 { Ok(()) } else { Err(MsgError::from_code(ret)) }
        })
    }

    pub fn set_code_int(&self, code: i32) -> Result<(), MsgError> {
        // 内部实现：转为字符串再设置
        let s = format!("{:05}", code);
        self.set_code(&s)
    }

    pub fn set_timestamp(&self, ts: Option<&str>) -> Result<(), MsgError> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnSetTimestamp> = lib.get(b"msg_set_timestamp").unwrap();
            let ret = if let Some(s) = ts {
                let cs = CString::new(s).unwrap();
                f(self.ptr, cs.as_ptr())
            } else {
                f(self.ptr, std::ptr::null())
            };
            if ret == 0 { Ok(()) } else { Err(MsgError::from_code(ret)) }
        })
    }

    // --------------------------------------------------------
    // Header 获取
    // --------------------------------------------------------

    pub fn msg_id(&self) -> String {
        with_lib(|lib| unsafe {
            let f: Symbol<FnGetMsgId> = lib.get(b"msg_get_msg_id").unwrap();
            read_c_str(f(self.ptr), 32)
        })
    }

    pub fn func(&self) -> String {
        with_lib(|lib| unsafe {
            let f: Symbol<FnGetFunc> = lib.get(b"msg_get_func").unwrap();
            read_c_str(f(self.ptr), 8).trim().to_string()
        })
    }

    pub fn code(&self) -> String {
        with_lib(|lib| unsafe {
            let f: Symbol<FnGetCode> = lib.get(b"msg_get_code").unwrap();
            read_c_str(f(self.ptr), 5)
        })
    }

    pub fn msg_type(&self) -> u8 {
        with_lib(|lib| unsafe {
            let f: Symbol<FnGetType> = lib.get(b"msg_get_type").unwrap();
            f(self.ptr)
        })
    }

    pub fn timestamp(&self) -> String {
        with_lib(|lib| unsafe {
            let f: Symbol<FnGetTimestamp> = lib.get(b"msg_get_timestamp").unwrap();
            read_c_str(f(self.ptr), 17)
        })
    }

    pub fn body_len(&self) -> u32 {
        with_lib(|lib| unsafe {
            let f: Symbol<FnGetBodyLen> = lib.get(b"msg_get_body_len").unwrap();
            f(self.ptr)
        })
    }

    pub fn total_len(&self) -> usize {
        with_lib(|lib| unsafe {
            let f: Symbol<FnGetTotalLen> = lib.get(b"msg_get_total_len").unwrap();
            f(self.ptr)
        })
    }

    // --------------------------------------------------------
    // 表头
    // --------------------------------------------------------

    pub fn set_headers(&self, ncols: i32, headers: &str) -> Result<(), MsgError> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnSetHeaders> = lib.get(b"msg_set_headers").unwrap();
            let s = CString::new(headers).unwrap();
            let ret = f(self.ptr, ncols, s.as_ptr());
            if ret == 0 { Ok(()) } else { Err(MsgError::from_code(ret)) }
        })
    }

    pub fn header_count(&self) -> usize {
        with_lib(|lib| unsafe {
            let f: Symbol<FnGetHeaderCount> = lib.get(b"msg_get_header_count").unwrap();
            f(self.ptr)
        })
    }

    pub fn get_headers(&self) -> String {
        with_lib(|lib| unsafe {
            let f: Symbol<FnGetHeaders> = lib.get(b"msg_get_headers").unwrap();
            let mut buf = vec![0u8; 4096];
            let mut len = 4096usize;
            f(self.ptr, buf.as_mut_ptr() as *mut c_char, &mut len);
            String::from_utf8_lossy(&buf[..len.min(4096)])
                .trim_end_matches('\0')
                .to_string()
        })
    }

    // --------------------------------------------------------
    // 数据行
    // --------------------------------------------------------

    pub fn add_row(&self) -> Result<(), MsgError> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnAddRow> = lib.get(b"msg_add_row").unwrap();
            let ret = f(self.ptr);
            if ret == 0 { Ok(()) } else { Err(MsgError::from_code(ret)) }
        })
    }

    pub fn set_value(&self, key: &str, value: &str) -> Result<(), MsgError> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnSetValueStr> = lib.get(b"msg_set_value_str").unwrap();
            let k = CString::new(key).unwrap();
            let v = CString::new(value).unwrap();
            let ret = f(self.ptr, k.as_ptr(), v.as_ptr());
            if ret == 0 { Ok(()) } else { Err(MsgError::from_code(ret)) }
        })
    }

    pub fn row_count(&self) -> usize {
        with_lib(|lib| unsafe {
            let f: Symbol<FnGetRowCount> = lib.get(b"msg_get_row_count").unwrap();
            f(self.ptr)
        })
    }

    // --------------------------------------------------------
    // 提交
    // --------------------------------------------------------

    pub fn finalize(&self) -> Result<(), MsgError> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnMsgFinalize> = lib.get(b"msg_finalize").unwrap();
            let ret = f(self.ptr);
            if ret == 0 { Ok(()) } else { Err(MsgError::from_code(ret)) }
        })
    }

    // --------------------------------------------------------
    // 游标遍历
    // --------------------------------------------------------

    pub fn fetch_next(&self) -> bool {
        with_lib(|lib| unsafe {
            let f: Symbol<FnFetchNext> = lib.get(b"msg_fetch_next").unwrap();
            f(self.ptr)
        })
    }

    pub fn reset_cursor(&self) {
        with_lib(|lib| unsafe {
            let f: Symbol<FnResetCursor> = lib.get(b"msg_reset_cursor").unwrap();
            f(self.ptr)
        })
    }

    pub fn current_row(&self) -> usize {
        with_lib(|lib| unsafe {
            let f: Symbol<FnGetCurrentRow> = lib.get(b"msg_get_current_row").unwrap();
            f(self.ptr)
        })
    }

    /// 按 key 获取当前游标行的值
    pub fn get_value(&self, key: &str) -> String {
        with_lib(|lib| unsafe {
            let f: Symbol<FnGetValueStr> = lib.get(b"msg_get_value_str").unwrap();
            let k = CString::new(key).unwrap();
            let mut val: *const c_char = std::ptr::null();
            let mut len = 0usize;
            f(self.ptr, k.as_ptr(), &mut val, &mut len);
            if val.is_null() || len == 0 {
                String::new()
            } else {
                read_c_str(val, len)
            }
        })
    }

    /// 按行列索引获取值
    pub fn get_field(&self, row: usize, col: usize) -> String {
        with_lib(|lib| unsafe {
            let f: Symbol<FnGetField> = lib.get(b"msg_get_field").unwrap();
            let mut val: *const c_char = std::ptr::null();
            let mut len = 0usize;
            f(self.ptr, row, col, &mut val, &mut len);
            if val.is_null() || len == 0 {
                String::new()
            } else {
                read_c_str(val, len)
            }
        })
    }

    // --------------------------------------------------------
    // 多结果集
    // --------------------------------------------------------

    /// 新增结果集并切换
    pub fn add_result_set(&self) -> bool {
        with_lib(|lib| unsafe {
            let f: Symbol<FnAddResultSet> = lib.get(b"msg_add_result_set").unwrap();
            f(self.ptr)
        })
    }

    /// 切换到下一结果集
    pub fn next_result_set(&self) -> bool {
        with_lib(|lib| unsafe {
            let f: Symbol<FnNextResultSet> = lib.get(b"msg_next_result_set").unwrap();
            f(self.ptr)
        })
    }

    /// 选择指定结果集（1-based）
    pub fn select_result_set(&self, rs: usize) -> Result<(), MsgError> {
        with_lib(|lib| unsafe {
            let f: Symbol<FnSelectResultSet> = lib.get(b"msg_select_result_set").unwrap();
            let ret = f(self.ptr, rs);
            if ret == 0 { Ok(()) } else { Err(MsgError::from_code(ret)) }
        })
    }

    /// 获取当前结果集编号（1-based）
    pub fn result_set(&self) -> usize {
        with_lib(|lib| unsafe {
            let f: Symbol<FnGetResultSet> = lib.get(b"msg_get_result_set").unwrap();
            f(self.ptr)
        })
    }

    /// 获取结果集数量
    pub fn result_set_count(&self) -> usize {
        with_lib(|lib| unsafe {
            let f: Symbol<FnGetResultSetCount> = lib.get(b"msg_get_result_set_count").unwrap();
            f(self.ptr)
        })
    }
}

impl Drop for Packet {
    fn drop(&mut self) {
        if self.owned && !self.ptr.is_null() {
            with_lib(|lib| unsafe {
                let f: Symbol<FnMsgDestroy> = lib.get(b"msg_destroy").unwrap();
                f(self.ptr)
            });
        }
    }
}

// ================================================================
// 库初始化
// ================================================================
pub fn load_library() -> Result<(), Box<dyn std::error::Error>> {
    let candidates = vec![
        // Windows
        "../../library/bin/x64/libmsgpacket.dll",
        "../library/bin/x64/libmsgpacket.dll",
        "library/bin/x64/libmsgpacket.dll",
        "libmsgpacket.dll",
        // Linux
        "../../library/bin/Lnx64/libmsgpacket.so",
        "../library/bin/Lnx64/libmsgpacket.so",
        "library/bin/Lnx64/libmsgpacket.so",
        "libmsgpacket.so",
        // macOS
        "../../library/bin/MacOS64/libmsgpacket.dylib",
        "../library/bin/MacOS64/libmsgpacket.dylib",
        "library/bin/MacOS64/libmsgpacket.dylib",
        "libmsgpacket.dylib",
    ];
    for path in &candidates {
        match unsafe { Library::new(path) } {
            Ok(lib) => {
                eprintln!("  [INFO] Loaded DLL: {}", path);
                unsafe { G_LIB = Some(lib) };
                return Ok(());
            }
            Err(_) => continue,
        }
    }
    Err("Cannot find libmsgpacket.dll".into())
}
