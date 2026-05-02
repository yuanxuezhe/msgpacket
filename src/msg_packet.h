#ifndef MSG_PACKET_H
#define MSG_PACKET_H

#include <stdint.h>
#include <stddef.h>
#include "msg_byteorder.h"

/* 消息魔数 */
#define MSG_MAGIC            "YSWY"
#define MSG_MAGIC_LEN        4

/* 默认版本 */
#define MSG_VERSION_DEFAULT  "V1.0"

/* 格式版本 */
#define MSG_FORMAT_TABLE     'T'  /* 0x54，表格式；可扩展 'J'=JSON */

/* 消息类型 */
#define MSG_TYPE_REQUEST     0x52   /* 'R' */
#define MSG_TYPE_ANSWER      0x41   /* 'A' */
#define MSG_TYPE_PUSH        0x50   /* 'P' */
#define MSG_TYPE_HEARTBEAT   0x48   /* 'H' */

/* 状态码（5 位数字字符串） */
#define MSG_CODE_SUCCESS     "00001"
#define MSG_CODE_ERROR       "99999"
#define MSG_CODE_TIMEOUT     "99998"

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

/* 消息头结构（不含 body_len，已提升到外层 msg_packet_t） */
typedef struct {
    char     msg_id[32];       /* 消息唯一标识 */
    char     ver[8];           /* 协议版本 */
    uint8_t  format;           /* 格式版本 */
    uint8_t  msg_type;         /* 消息类型 */
    char     timestamp[17];    /* yyyyMMddHHmmssSSS，17位无\0 */
    char     func[8];          /* 函数/操作名 */
    char     msg_code[5];      /* 5位状态码，如 "00001" */
} msg_header_t;  /* 72 字节（packed） */

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
#define HEAD_VER_POS         offsetof(msg_packet_t, header.ver)        /* 44 */
#define HEAD_VER_LENGTH      8
#define HEAD_FORMAT_POS      offsetof(msg_packet_t, header.format)     /* 52 */
#define HEAD_FORMAT_LENGTH   1
#define HEAD_MSGTYPE_POS     offsetof(msg_packet_t, header.msg_type)   /* 53 */
#define HEAD_MSGTYPE_LENGTH  1
#define HEAD_TIMESTAMP_POS   offsetof(msg_packet_t, header.timestamp)  /* 54 */
#define HEAD_TIMESTAMP_LENGTH 17
#define HEAD_FUNC_POS        offsetof(msg_packet_t, header.func)       /* 71 */
#define HEAD_FUNC_LENGTH     8
#define HEAD_CODE_POS        offsetof(msg_packet_t, header.msg_code)   /* 79 */
#define HEAD_CODE_LENGTH     5
#define HEAD_SIZE            sizeof(msg_header_t)                      /* 72 (packed) */

/* body 起始偏移：magic[4] + crc32[4] + body_len[4] + header[HEAD_SIZE] = 84 */
#define BODY_OFFSET          offsetof(msg_packet_t, body)

#endif /* MSG_PACKET_H */
