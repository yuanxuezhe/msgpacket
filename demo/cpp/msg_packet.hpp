/**
 * msg_packet.hpp — MsgPacket 的轻量 RAII C++ 封装
 *
 * 直接委托 C API，提供资源自动管理和类型安全。
 * 不重新实现任何协议逻辑，仅封装生命周期。
 */

#ifndef MSG_PACKET_HPP
#define MSG_PACKET_HPP

#include <string>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <ctime>

extern "C" {
#include "msg_api.h"
}

namespace msgpacket {

/* ================================================================
 * 消息类型常量（C++ 风格）
 * ================================================================ */
enum class MsgType : uint8_t {
    REQUEST  = MSG_TYPE_REQUEST,   // 'R'
    ANSWER   = MSG_TYPE_ANSWER,    // 'A'
    PUSH     = MSG_TYPE_PUSH,      // 'P'
    HEARTBEAT = MSG_TYPE_HEARTBEAT // 'H'
};

inline const char* msg_type_name(MsgType t) {
    switch (t) {
    case MsgType::REQUEST:   return "REQUEST";
    case MsgType::ANSWER:    return "ANSWER";
    case MsgType::PUSH:      return "PUSH";
    case MsgType::HEARTBEAT: return "HEARTBEAT";
    default:                 return "UNKNOWN";
    }
}

/* ================================================================
 * MsgPacket — RAII 封装类
 *
 * 用法:
 *   MsgPacket pkt(MsgType::REQUEST);
 *   pkt.set_func("getData");
 *   pkt.set_headers(3, "Symbol,Price,Volume");
 *   pkt.add_row();
 *   pkt.set_value("Symbol", "BTC/USDT");
 *   pkt.finalize();
 *   auto data = pkt.wire_data();
 * ================================================================ */
class MsgPacket {
public:
    /* 构造：创建新数据包 */
    explicit MsgPacket(MsgType type, const char* ver = "V1.0")
        : ptr_(msg_create(static_cast<uint8_t>(type), ver))
    {
        if (!ptr_) throw std::runtime_error("msg_create failed");
    }

    /* 析构 */
    ~MsgPacket() { if (ptr_) msg_destroy(ptr_); }

    /* 禁止拷贝（因为 ptr_ 是独占资源） */
    MsgPacket(const MsgPacket&) = delete;
    MsgPacket& operator=(const MsgPacket&) = delete;

    /* 允许移动 */
    MsgPacket(MsgPacket&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }
    MsgPacket& operator=(MsgPacket&& other) noexcept {
        if (this != &other) {
            if (ptr_) msg_destroy(ptr_);
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    /* 获取原始 C 指针（用于与 C API 互操作） */
    msg_packet_t* c_ptr() const { return ptr_; }

    /* ============================================================
     * Header 设置
     * ============================================================ */
    void set_msg_id(const char* id)    { msg_set_msg_id(ptr_, id); }
    void set_func(const char* f)       { msg_set_func(ptr_, f); }
    void set_timestamp(const char* ts = nullptr) { msg_set_timestamp(ptr_, ts); }
    void set_format(uint8_t fmt)       { msg_set_format(ptr_, fmt); }
    void set_version(const char* ver)  { msg_set_version(ptr_, ver); }

    /* ============================================================
     * Header 获取
     * ============================================================ */
    std::string msg_id()   const { return std::string(msg_get_msg_id(ptr_), 32); }
    std::string func()     const { return str_trimmed(msg_get_func(ptr_), 8); }
    std::string version()  const { return str_trimmed(msg_get_version(ptr_), 8); }
    uint8_t     type()     const { return msg_get_type(ptr_); }
    std::string timestamp() const { return std::string(msg_get_timestamp(ptr_), 17); }
    uint8_t     format()   const { return msg_get_format(ptr_); }
    uint32_t    body_len() const { return msg_get_body_len(ptr_); }
    size_t      total_len() const { return msg_get_total_len(ptr_); }
    size_t      header_count() const { return msg_get_header_count(ptr_); }
    size_t      row_count() const { return msg_get_row_count(ptr_); }

    /* ============================================================
     * 表头
     * ============================================================ */
    void set_headers(int ncols, const char* headers) {
        msg_set_headers(ptr_, ncols, headers);
    }

    std::string get_headers() const {
        char buf[4096] = {};
        size_t len = sizeof(buf);
        msg_get_headers(ptr_, buf, &len);
        return std::string(buf, len);
    }

    /* ============================================================
     * 多结果集支持
     * ============================================================ */
    /* 新增结果集并切换 */
    bool add_result_set() { return msg_add_result_set(ptr_) != 0; }
    /* 切换到下一结果集 */
    bool next_result_set() { return msg_next_result_set(ptr_) != 0; }
    /* 选择指定结果集（1-based） */
    int select_result_set(size_t rs_number) { return msg_select_result_set(ptr_, rs_number); }
    /* 获取当前结果集编号（1-based） */
    size_t result_set() const { return msg_get_result_set(ptr_); }
    /* 获取结果集数量 */
    size_t result_set_count() const { return msg_get_result_set_count(ptr_); }

    /* ============================================================
     * 数据行
     * ============================================================ */
    void add_row() { msg_add_row(ptr_); }

    /* Key-Value 设值（推荐使用，类型安全） */
    void set_value(const char* key, const char* value) {
        msg_set_value_str(ptr_, key, value);
    }
    void set_value(const char* key, int32_t value) {
        msg_set_value_i32(ptr_, key, value);
    }
    void set_value(const char* key, int64_t value) {
        msg_set_value_i64(ptr_, key, value);
    }
    void set_value(const char* key, double value) {
        msg_set_value_double(ptr_, key, value);
    }

    void clear_rows() { msg_clear_rows(ptr_); }

    /* ============================================================
     * 提交与获取
     * ============================================================ */
    int finalize() { return msg_finalize(ptr_); }

    const void* wire_data() const { return msg_data(ptr_); }
    size_t      wire_size() const { return msg_size(ptr_); }

    /* ============================================================
     * 编码/解码（返回独立副本）
     * ============================================================ */
    static MsgPacket decode(const void* buf, size_t len) {
        msg_packet_t* p = nullptr;
        int ret = msg_decode(buf, len, &p);
        if (ret != 0) {
            throw std::runtime_error(
                std::string("msg_decode failed: ") + error_name(ret));
        }
        MsgPacket pkt;
        pkt.ptr_ = p;
        return pkt;
    }

    /* ============================================================
     * 游标遍历
     * ============================================================ */
    bool fetch_next()         { return msg_fetch_next(ptr_); }
    void reset_cursor()       { msg_reset_cursor(ptr_); }
    size_t current_row() const { return msg_get_current_row(ptr_); }

    /* ============================================================
     * 字段获取（按 key，当前游标行）
     * ============================================================ */
    std::string get_value_str(const char* key) const {
        const char* val = nullptr;
        size_t len = 0;
        msg_get_value_str(ptr_, key, &val, &len);
        return val ? std::string(val, len) : std::string();
    }

    bool get_value(const char* key, int32_t& out) const {
        return msg_get_value_i32(ptr_, key, &out) == 0;
    }
    bool get_value(const char* key, int64_t& out) const {
        return msg_get_value_i64(ptr_, key, &out) == 0;
    }
    bool get_value(const char* key, double& out) const {
        return msg_get_value_double(ptr_, key, &out) == 0;
    }

    /* 按行列索引获取 */
    std::string get_field(size_t row, size_t col) const {
        const char* val = nullptr;
        size_t len = 0;
        msg_get_field(ptr_, row, col, &val, &len);
        return val ? std::string(val, len) : std::string();
    }

    /* ============================================================
     * 错误信息
     * ============================================================ */
    static const char* error_name(int err) {
        switch (err) {
        case MSG_ERR_INVALID_MAGIC:   return "INVALID_MAGIC";
        case MSG_ERR_CRC_MISMATCH:    return "CRC_MISMATCH";
        case MSG_ERR_BODY_TOO_LARGE:  return "BODY_TOO_LARGE";
        case MSG_ERR_TOO_MANY_HEADERS: return "TOO_MANY_HEADERS";
        case MSG_ERR_TOO_MANY_ROWS:   return "TOO_MANY_ROWS";
        case MSG_ERR_FIELD_TOO_LONG:  return "FIELD_TOO_LONG";
        case MSG_ERR_ESCAPE_SEQUENCE: return "ESCAPE_SEQUENCE";
        default:                      return "UNKNOWN";
        }
    }

private:
    msg_packet_t* ptr_;

    /* 内部构造：接管已有指针（从 decode 返回） */
    MsgPacket() : ptr_(nullptr) {}

    /* 去除尾部空白/填充 \0 */
    static std::string str_trimmed(const char* s, size_t max_len) {
        size_t len = 0;
        for (size_t i = 0; i < max_len && s[i]; i++) len++;
        return std::string(s, len);
    }
};

} // namespace msgpacket

#endif // MSG_PACKET_HPP
