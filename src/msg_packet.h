#ifndef MSG_PACKET_H
#define MSG_PACKET_H

#include <stdint.h>
#include <stddef.h>

/* 字节序检测和转换宏（统一使用小端序） */
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    #define MSG_IS_BIG_ENDIAN    1
    #if defined(__GNUC__) || defined(__clang__)
        #define MSG_HTOLE64(x)   __builtin_bswap64(x)
        #define MSG_HTOLE32(x)   __builtin_bswap32(x)
        #define MSG_LE64TOH(x)   __builtin_bswap64(x)
        #define MSG_LE32TOH(x)   __builtin_bswap32(x)
    #elif defined(_MSC_VER)
        #include <stdlib.h>
        #define MSG_HTOLE64(x)   _byteswap_uint64(x)
        #define MSG_HTOLE32(x)   _byteswap_ulong(x)
        #define MSG_LE64TOH(x)   _byteswap_uint64(x)
        #define MSG_LE32TOH(x)   _byteswap_ulong(x)
    #else
        static inline uint64_t msg_swap64(uint64_t x) {
            return (((x) & 0xFF00000000000000ULL) >> 56) |
                   (((x) & 0x00FF000000000000ULL) >> 40) |
                   (((x) & 0x0000FF0000000000ULL) >> 24) |
                   (((x) & 0x000000FF00000000ULL) >> 8)  |
                   (((x) & 0x00000000FF000000ULL) << 8)  |
                   (((x) & 0x0000000000FF0000ULL) << 24) |
                   (((x) & 0x000000000000FF00ULL) << 40) |
                   (((x) & 0x00000000000000FFULL) << 56);
        }
        static inline uint32_t msg_swap32(uint32_t x) {
            return (((x) & 0xFF000000UL) >> 24) |
                   (((x) & 0x00FF0000UL) >> 8)  |
                   (((x) & 0x0000FF00UL) << 8)  |
                   (((x) & 0x000000FFUL) << 24);
        }
        #define MSG_HTOLE64(x)   msg_swap64(x)
        #define MSG_HTOLE32(x)   msg_swap32(x)
        #define MSG_LE64TOH(x)   msg_swap64(x)
        #define MSG_LE32TOH(x)   msg_swap32(x)
    #endif
#else
    /* Little-Endian 平台（x86/x64/ARM LE 等） */
    #define MSG_IS_BIG_ENDIAN    0
    #define MSG_HTOLE64(x)       (x)
    #define MSG_HTOLE32(x)       (x)
    #define MSG_LE64TOH(x)       (x)
    #define MSG_LE32TOH(x)       (x)
#endif

/* 消息魔数 */
#define MSG_MAGIC            "YSWY"
#define MSG_MAGIC_LEN        4
#define MSG_CRC32_SIZE       4
#define MSG_BODY_LEN_SIZE    4
#define MSG_PRE_HEADER_SIZE  (MSG_MAGIC_LEN + MSG_CRC32_SIZE + MSG_BODY_LEN_SIZE)  /* 12 */

/* 格式版本 */
#define MSG_FORMAT_TABLE     'T'  /* 0x54，表格式；可扩展 'J'=JSON */

/* ========== 版本信息 ========== */
#define MSG_VERSION_MAJOR    1
#define MSG_VERSION_MINOR    1
#define MSG_VERSION_PATCH    0
#define MSG_VERSION_STRING   "1.1.0"

/* 默认版本（兼容旧格式）*/
#define MSG_VERSION_DEFAULT  "V1.0"

/* 消息类型 */
#define MSG_TYPE_REQUEST     0x52   /* 'R' */
#define MSG_TYPE_ANSWER      0x41   /* 'A' */
#define MSG_TYPE_PUSH        0x50   /* 'P' */
#define MSG_TYPE_HEARTBEAT   0x48   /* 'H' */

/* 状态码（已废弃，msg_code 字段已移除） */
/* MSG_CODE_SUCCESS/ERROR/TIMEOUT 已删除 */

/* 字段数量上限 */
#define MSG_MAX_HEADERS      256
#define MSG_MAX_ROWS        65536
#define MSG_MAX_FIELD_LEN    4096
#define MSG_MAX_BODY_LEN     (1024 * 1024)

/* 转义序列 */
#define MSG_ESC_CHAR_US     0x5F   /* '_' -> 0x1F */
#define MSG_ESC_CHAR_RS     0x5E   /* '^' -> 0x1E */
#define MSG_ESC_CHAR_FS     0x5C   /* '\' -> 0x1C */
#define MSG_ESC_CHAR_ESC    0x5B   /* '[' -> 0x1B */
#define MSG_ESC_CHAR_GS     0x5D   /* ']' -> 0x1D */

/* 分隔符 */
#define MSG_SEP_COL         0x1F   /* US - 列分隔 */
#define MSG_SEP_ROW         0x1E   /* RS - 行分隔 */
#define MSG_SEP_SECTION     0x1C   /* FS - 区隔表头与数据 */
#define MSG_SEP_RS_GROUP    0x1D   /* GS - 结果集分隔（ANSWER 包多结果集） */

/* 错误码 */
#define MSG_ERR_NULL_PTR           -1
#define MSG_ERR_INVALID_MAGIC     -2
#define MSG_ERR_CRC_MISMATCH      -3
#define MSG_ERR_BUFFER_TOO_SMALL  -4
#define MSG_ERR_INVALID_FORMAT    -5
#define MSG_ERR_INVALID_MSG_TYPE  -6
#define MSG_ERR_ESCAPE_SEQUENCE   -7
#define MSG_ERR_NO_DATA           -8
#define MSG_ERR_BODY_TOO_LARGE    -9
#define MSG_ERR_TOO_MANY_HEADERS  -10
#define MSG_ERR_TOO_MANY_ROWS     -11
#define MSG_ERR_FIELD_TOO_LONG    -12
#define MSG_ERR_VERSION_MISMATCH  -13
#define MSG_ERR_NO_MEMORY         -14
#define MSG_ERR_NOT_FINALIZED     -15

#pragma pack(push, 1)

/* 消息头结构（不含 body_len，已提升到外层 msg_packet_t）
 * 所有字符串字段固定长度 + \0 终止，最后一位一定为 0 */
typedef struct {
    char     msg_id[33];       /* 消息唯一标识，32字节UUID + \0 */
    char     ver[9];           /* 协议版本，最大8字节 + \0 */
    uint8_t  format;           /* 格式版本 */
    uint8_t  msg_type;         /* 消息类型 */
    char     timestamp[18];    /* yyyyMMddHHmmssSSS，17位 + \0 */
    char     func[9];           /* 函数/操作名，最大8字节 + \0 */
} msg_header_t;  /* 71 字节（packed） */

/* 数据包结构（线上格式 + 柔性数组，不含运行时状态）
 * wire 布局：magic[4] + crc32[4] + body_len[4] + header[HEAD_SIZE] + body[] */
typedef struct {
    char          magic[4];    /* 固定 "YSWY" */
    uint32_t      crc32;       /* CRC32 */
    uint32_t      body_len;    /* Body 字节数（wire 上为转义后长度，小端序） */
    msg_header_t  header;      /* 消息头 */
    uint8_t       body[];      /* 柔性数组（C99），包体数据紧跟 header 之后 */
} msg_packet_t;

#pragma pack(pop)

/* wire 缓冲区中的字段偏移量（从 buf 起点 byte 0 起算） */
#define BODY_LEN_POS         offsetof(msg_packet_t, body_len)          /* 8 */
#define BODY_LEN_LENGTH      4
#define HEAD_MSGID_POS       offsetof(msg_packet_t, header.msg_id)     /* 12 */
#define HEAD_MSGID_LENGTH    32
#define HEAD_VER_POS         offsetof(msg_packet_t, header.ver)        /* 45 */
#define HEAD_VER_LENGTH      8
#define HEAD_FORMAT_POS      offsetof(msg_packet_t, header.format)     /* 54 */
#define HEAD_FORMAT_LENGTH   1
#define HEAD_MSGTYPE_POS     offsetof(msg_packet_t, header.msg_type)   /* 55 */
#define HEAD_MSGTYPE_LENGTH  1
#define HEAD_TIMESTAMP_POS   offsetof(msg_packet_t, header.timestamp)  /* 56 */
#define HEAD_TIMESTAMP_LENGTH 18
#define HEAD_FUNC_POS        offsetof(msg_packet_t, header.func)       /* 74 */
#define HEAD_FUNC_LENGTH     8
/* HEAD_CODE_POS/HEAD_CODE_LENGTH 已移除（msg_code 字段已删除） */
#define HEAD_SIZE            sizeof(msg_header_t)                      /* 71 (packed) */

/* body 起始偏移：magic[4] + crc32[4] + body_len[4] + header[HEAD_SIZE] = 83 */
#define BODY_OFFSET          offsetof(msg_packet_t, body)

#endif /* MSG_PACKET_H */
