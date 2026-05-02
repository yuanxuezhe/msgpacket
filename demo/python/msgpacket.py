"""
msgpacket.py — MsgPacket ctypes 绑定

支持 Windows (libmsgpacket.dll) 和 Linux (libmsgpacket.so)。
提供 Pythonic 的 MsgPacket 类封装。
"""

import ctypes
from ctypes import (
    c_void_p, c_char_p, c_uint8, c_int32, c_int64, c_uint32, c_size_t,
    c_char, c_bool, POINTER, byref, create_string_buffer, cast, addressof,
    Structure, string_at
)
import os
import sys
from typing import Optional, Tuple, List

# ================================================================
# 常量
# ================================================================
MSG_MAGIC          = b"YSWY"
MSG_VERSION_DEFAULT = "V1.0"
MSG_FORMAT_TABLE   = 0x54  # 'T'

MSG_TYPE_REQUEST   = 0x52  # 'R'
MSG_TYPE_ANSWER    = 0x41  # 'A'
MSG_TYPE_PUSH      = 0x50  # 'P'
MSG_TYPE_HEARTBEAT = 0x48  # 'H'

MSG_CODE_SUCCESS   = "00001"
MSG_CODE_ERROR     = "99999"
MSG_CODE_TIMEOUT   = "99998"

HEAD_SIZE = 72  # sizeof(msg_header_t) packed
BODY_OFFSET = 4 + 4 + 4 + HEAD_SIZE  # magic + crc32 + body_len + header = 84

# ================================================================
# 加载动态库
# ================================================================
def _find_lib():
    """查找 msgpacket 动态库"""
    base = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    if sys.platform == "win32":
        paths = [
            os.path.join(base, "library", "bin", "x64", "libmsgpacket.dll"),
            os.path.join(base, "build", "demo", "c", "libmsgpacket.dll"),
            os.path.join(base, "build", "libmsgpacket.dll"),
        ]
    else:
        paths = [
            os.path.join(base, "library", "bin", "Lnx64", "libmsgpacket.so"),
        ]
    for p in paths:
        if os.path.exists(p):
            return p
    return None

_lib_path = _find_lib()
if _lib_path:
    _lib = ctypes.CDLL(_lib_path)
else:
    # fallback: try system path
    _lib = ctypes.CDLL("libmsgpacket.dll" if sys.platform == "win32" else "libmsgpacket.so")


# ================================================================
# 函数签名定义
# ================================================================

# 创建/销毁
_lib.msg_create.argtypes = [c_uint8, c_char_p]
_lib.msg_create.restype = c_void_p

_lib.msg_destroy.argtypes = [c_void_p]
_lib.msg_destroy.restype = None

_lib.msg_clone.argtypes = [c_void_p]
_lib.msg_clone.restype = c_void_p

# Header 设置
_lib.msg_set_func.argtypes = [c_void_p, c_char_p]
_lib.msg_set_func.restype = c_int32

_lib.msg_set_code.argtypes = [c_void_p, c_char_p]
_lib.msg_set_code.restype = c_int32

_lib.msg_set_timestamp.argtypes = [c_void_p, c_char_p]
_lib.msg_set_timestamp.restype = c_int32

_lib.msg_set_version.argtypes = [c_void_p, c_char_p]
_lib.msg_set_version.restype = c_int32

_lib.msg_set_msg_id.argtypes = [c_void_p, c_char_p]
_lib.msg_set_msg_id.restype = c_int32

# Header 获取
_lib.msg_get_msg_id.argtypes = [c_void_p]
_lib.msg_get_msg_id.restype = c_char_p

_lib.msg_get_func.argtypes = [c_void_p]
_lib.msg_get_func.restype = c_char_p

_lib.msg_get_version.argtypes = [c_void_p]
_lib.msg_get_version.restype = c_char_p

_lib.msg_get_type.argtypes = [c_void_p]
_lib.msg_get_type.restype = c_uint8

_lib.msg_get_code.argtypes = [c_void_p]
_lib.msg_get_code.restype = c_char_p

_lib.msg_get_timestamp.argtypes = [c_void_p]
_lib.msg_get_timestamp.restype = c_char_p

_lib.msg_get_format.argtypes = [c_void_p]
_lib.msg_get_format.restype = c_uint8

_lib.msg_get_body_len.argtypes = [c_void_p]
_lib.msg_get_body_len.restype = c_uint32

_lib.msg_get_total_len.argtypes = [c_void_p]
_lib.msg_get_total_len.restype = c_size_t

# 表头
_lib.msg_set_headers.argtypes = [c_void_p, c_int32, c_char_p]
_lib.msg_set_headers.restype = c_int32

_lib.msg_add_header.argtypes = [c_void_p, c_char_p]
_lib.msg_add_header.restype = c_int32

_lib.msg_get_headers.argtypes = [c_void_p, c_char_p, POINTER(c_size_t)]
_lib.msg_get_headers.restype = c_int32

# 数据行
_lib.msg_begin_row.argtypes = [c_void_p]
_lib.msg_begin_row.restype = c_int32

_lib.msg_set_value_str.argtypes = [c_void_p, c_char_p, c_char_p]
_lib.msg_set_value_str.restype = c_int32

_lib.msg_set_value_i32.argtypes = [c_void_p, c_char_p, c_int32]
_lib.msg_set_value_i32.restype = c_int32

_lib.msg_set_value_i64.argtypes = [c_void_p, c_char_p, c_int64]
_lib.msg_set_value_i64.restype = c_int32

_lib.msg_set_value_double.argtypes = [c_void_p, c_char_p, ctypes.c_double]
_lib.msg_set_value_double.restype = c_int32

_lib.msg_clear_rows.argtypes = [c_void_p]
_lib.msg_clear_rows.restype = c_int32

# 提交
_lib.msg_finalize.argtypes = [c_void_p]
_lib.msg_finalize.restype = c_int32

_lib.msg_data.argtypes = [c_void_p]
_lib.msg_data.restype = c_void_p

_lib.msg_size.argtypes = [c_void_p]
_lib.msg_size.restype = c_size_t

# 编码/解码
_lib.msg_encode.argtypes = [c_void_p, POINTER(c_void_p), POINTER(c_size_t)]
_lib.msg_encode.restype = c_int32

_lib.msg_decode.argtypes = [c_void_p, c_size_t, POINTER(c_void_p)]
_lib.msg_decode.restype = c_int32

_lib.msg_free_buffer.argtypes = [c_void_p]
_lib.msg_free_buffer.restype = None

# 游标遍历
_lib.msg_fetch_next.argtypes = [c_void_p]
_lib.msg_fetch_next.restype = c_bool

_lib.msg_reset_cursor.argtypes = [c_void_p]
_lib.msg_reset_cursor.restype = None

_lib.msg_get_current_row.argtypes = [c_void_p]
_lib.msg_get_current_row.restype = c_size_t

# 字段获取
_lib.msg_get_value_str.argtypes = [c_void_p, c_char_p, POINTER(c_char_p), POINTER(c_size_t)]
_lib.msg_get_value_str.restype = c_int32

_lib.msg_get_value_i32.argtypes = [c_void_p, c_char_p, POINTER(c_int32)]
_lib.msg_get_value_i32.restype = c_int32

_lib.msg_get_value_i64.argtypes = [c_void_p, c_char_p, POINTER(c_int64)]
_lib.msg_get_value_i64.restype = c_int32

_lib.msg_get_value_double.argtypes = [c_void_p, c_char_p, POINTER(ctypes.c_double)]
_lib.msg_get_value_double.restype = c_int32

_lib.msg_get_field.argtypes = [c_void_p, c_size_t, c_size_t, POINTER(c_char_p), POINTER(c_size_t)]
_lib.msg_get_field.restype = c_int32

# 统计
_lib.msg_get_header_count.argtypes = [c_void_p]
_lib.msg_get_header_count.restype = c_size_t

_lib.msg_get_row_count.argtypes = [c_void_p]
_lib.msg_get_row_count.restype = c_size_t


# ================================================================
# Pythonic 封装类
# ================================================================
class MsgPacket:
    """MsgPacket — Pythonic 封装

    用法:
        pkt = MsgPacket(MSG_TYPE_REQUEST, "V1.0")
        pkt.set_func("getData")
        pkt.set_headers(4, "Symbol,Price,Volume,Time")
        pkt.begin_row()
        pkt.set_value("Symbol", "BTC/USDT")
        pkt.finalize()
        raw = pkt.wire_data_bytes()
    """

    def __init__(self, msg_type: int = None, version: str = "V1.0", ptr: int = None):
        if ptr is not None:
            self._ptr = ptr
        elif msg_type is not None:
            self._ptr = _lib.msg_create(msg_type, version.encode())
            if not self._ptr:
                raise MemoryError("msg_create failed")
        else:
            raise ValueError("Either msg_type or ptr must be provided")

    def __del__(self):
        if hasattr(self, '_ptr') and self._ptr:
            _lib.msg_destroy(self._ptr)
            self._ptr = None

    @property
    def ptr(self) -> int:
        return self._ptr

    # --- Header 设置 ---
    def set_func(self, f: str):         _lib.msg_set_func(self._ptr, f.encode())
    def set_code(self, code: str):      _lib.msg_set_code(self._ptr, code.encode() if code else None)
    def set_timestamp(self, ts: str=None): _lib.msg_set_timestamp(self._ptr, ts.encode() if ts else None)
    def set_version(self, ver: str):    _lib.msg_set_version(self._ptr, ver.encode())
    def set_msg_id(self, mid: str):     _lib.msg_set_msg_id(self._ptr, mid.encode())

    # --- Header 获取 ---
    def msg_id(self) -> str:
        ptr = _lib.msg_get_msg_id(self._ptr)
        if not ptr:
            return ""
        return string_at(ptr, 32).decode('utf-8', errors='replace').rstrip('\0')

    def func(self) -> str:
        ptr = _lib.msg_get_func(self._ptr)
        if not ptr:
            return ""
        raw = string_at(ptr, 8)
        # 截取到第一个 \0
        end = raw.find(b'\0')
        return raw[:end].decode('utf-8', errors='replace') if end >= 0 else raw.decode('utf-8', errors='replace')

    def version(self) -> str:
        ptr = _lib.msg_get_version(self._ptr)
        if not ptr:
            return ""
        raw = string_at(ptr, 8)
        end = raw.find(b'\0')
        return raw[:end].decode('utf-8', errors='replace') if end >= 0 else raw.decode('utf-8', errors='replace')
    def msg_type(self) -> int:       return _lib.msg_get_type(self._ptr)
    def code(self) -> str:
        ptr = _lib.msg_get_code(self._ptr)
        return string_at(ptr, 5).decode('utf-8', errors='replace') if ptr else ""
    def timestamp(self) -> str:
        ptr = _lib.msg_get_timestamp(self._ptr)
        return string_at(ptr, 17).decode('utf-8', errors='replace') if ptr else ""
    def format(self) -> int:         return _lib.msg_get_format(self._ptr)
    def body_len(self) -> int:       return _lib.msg_get_body_len(self._ptr)
    def total_len(self) -> int:      return _lib.msg_get_total_len(self._ptr)
    def header_count(self) -> int:   return _lib.msg_get_header_count(self._ptr)
    def row_count(self) -> int:      return _lib.msg_get_row_count(self._ptr)

    def msg_type_name(self) -> str:
        m = {MSG_TYPE_REQUEST: "REQUEST", MSG_TYPE_ANSWER: "ANSWER",
             MSG_TYPE_PUSH: "PUSH", MSG_TYPE_HEARTBEAT: "HEARTBEAT"}
        return m.get(self.msg_type(), "UNKNOWN")

    # --- 表头 ---
    def set_headers(self, ncols: int, headers: str):
        _lib.msg_set_headers(self._ptr, ncols, headers.encode())

    def get_headers(self) -> str:
        buf = create_string_buffer(4096)
        size = c_size_t(4096)
        _lib.msg_get_headers(self._ptr, buf, byref(size))
        actual_len = size.value
        if actual_len > 0 and actual_len <= 4096:
            return buf.raw[:actual_len].decode('utf-8', errors='replace').rstrip('\0')
        return ""

    # --- 数据行 ---
    def begin_row(self): _lib.msg_begin_row(self._ptr)

    def set_value_str(self, key: str, value: str):
        _lib.msg_set_value_str(self._ptr, key.encode(), value.encode())

    def set_value_i32(self, key: str, value: int):
        _lib.msg_set_value_i32(self._ptr, key.encode(), value)

    def set_value_i64(self, key: str, value: int):
        _lib.msg_set_value_i64(self._ptr, key.encode(), value)

    def set_value_double(self, key: str, value: float):
        _lib.msg_set_value_double(self._ptr, key.encode(), value)

    def set_value(self, key: str, value):
        """统一 set_value，根据类型自动选择"""
        if isinstance(value, str):
            self.set_value_str(key, value)
        elif isinstance(value, float):
            self.set_value_double(key, value)
        elif isinstance(value, int):
            if value < -2147483648 or value > 2147483647:
                self.set_value_i64(key, value)
            else:
                self.set_value_i32(key, value)
        else:
            self.set_value_str(key, str(value))

    def clear_rows(self): _lib.msg_clear_rows(self._ptr)

    # --- 提交 ---
    def finalize(self) -> int:
        return _lib.msg_finalize(self._ptr)

    def wire_data_ptr(self) -> int:
        p = _lib.msg_data(self._ptr)
        return p if isinstance(p, int) else (p.value if p else 0)

    def wire_size(self) -> int:
        return _lib.msg_size(self._ptr)

    def wire_data_bytes(self) -> bytes:
        size = self.wire_size()
        if size == 0:
            return b""
        ptr = self.wire_data_ptr()
        buf = (ctypes.c_uint8 * size).from_address(ptr)
        return bytes(buf)

    # --- 编码/解码 ---
    def encode(self) -> Tuple[int, bytes]:
        out_buf = c_void_p()
        out_len = c_size_t()
        ret = _lib.msg_encode(self._ptr, byref(out_buf), byref(out_len))
        if ret != 0:
            return (ret, b"")
        size = out_len.value
        buf = (ctypes.c_uint8 * size).from_address(out_buf.value)
        data = bytes(buf)
        _lib.msg_free_buffer(out_buf)
        return (0, data)

    @staticmethod
    def decode(data: bytes) -> "MsgPacket":
        buf = (ctypes.c_uint8 * len(data))(*data)
        out = c_void_p()
        ret = _lib.msg_decode(
            cast(byref(buf), c_void_p),
            c_size_t(len(data)),
            byref(out)
        )
        if ret != 0:
            raise RuntimeError(f"msg_decode failed: error={ret}")
        return MsgPacket(ptr=out.value)

    # --- 游标遍历 ---
    def fetch_next(self) -> bool:    return _lib.msg_fetch_next(self._ptr)
    def reset_cursor(self):          _lib.msg_reset_cursor(self._ptr)
    def current_row(self) -> int:    return _lib.msg_get_current_row(self._ptr)

    # --- 字段获取 ---
    def get_value_str(self, key: str) -> str:
        val = c_char_p()
        val_len = c_size_t()
        ret = _lib.msg_get_value_str(self._ptr, key.encode(), byref(val), byref(val_len))
        if ret != 0 or not val.value:
            return ""
        return val.value[:val_len.value].decode()

    def get_field(self, row: int, col: int) -> str:
        val = c_char_p()
        val_len = c_size_t()
        ret = _lib.msg_get_field(self._ptr, row, col, byref(val), byref(val_len))
        if ret != 0 or not val.value:
            return ""
        return val.value[:val_len.value].decode()


# ================================================================
# 辅助：类型名
# ================================================================
def msg_type_name(t: int) -> str:
    m = {MSG_TYPE_REQUEST: "REQUEST", MSG_TYPE_ANSWER: "ANSWER",
         MSG_TYPE_PUSH: "PUSH", MSG_TYPE_HEARTBEAT: "HEARTBEAT"}
    return m.get(t, "UNKNOWN")
