#include "msg_packet.h"
#include "msg_byteorder.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* CRC32 多项式（IEEE 802.3） */
#define CRC32_POLY  0xEDB88320UL
#define CRC32_INIT  0xFFFFFFFFUL

static uint32_t crc32_table[256];
static volatile int crc32_initialized = 0;

/* UUID v4 生成 */
void msg_generate_uuid_v4(char out[32]) {
    uint8_t bytes[16];
    static const char hex[] = "0123456789ABCDEF";
    static uint32_t seed = 0;

    /*
     * 使用静态持久化种子替代每次都从 time(NULL) 初始化，
     * 避免短时间内多次调用生成完全相同 UUID。
     * 首次调用时混入时间戳和地址噪声。
     * 注意：生产环境应使用加密安全随机数生成器。
     */
    if (seed == 0) {
        seed = (uint32_t)time(NULL) ^ (uint32_t)((uintptr_t)&seed);
    }

    for (int i = 0; i < 16; i++) {
        /* 简单线性同余生成器 */
        seed = seed * 1103515245UL + 12345UL;
        bytes[i] = (uint8_t)(seed >> 16);
    }

    /* 设置版本（4）和变体（8/9/a/b） */
    bytes[6] = (bytes[6] & 0x0F) | 0x40;
    bytes[8] = (bytes[8] & 0x3F) | 0x80;

    /* 转换为十六进制字符串 */
    for (int i = 0; i < 16; i++) {
        out[i * 2]     = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 0x0F];
    }
}

/* 初始化 CRC32 表（线程安全）
 * 注意：多线程首次并发调用时存在微小竞态窗口——T1 完成 init 前 T2
 * 可能已通过 test-and-set 返回并使用未就绪的表。由于 init 仅 256
 * 次迭代（微秒级），此窗口极窄。生产环境建议在程序启动时单线程调用一次。 */
void crc32_init(void) {
    if (crc32_initialized) return;

#if defined(_MSC_VER)
    /* MSVC: InterlockedExchange 返回旧值，若已被其他线程设置则直接返回 */
    if (_InterlockedExchange((volatile long*)&crc32_initialized, 1) != 0) {
        return;
    }
#else
    /* GCC/Clang: 使用原子 test-and-set */
    if (__sync_lock_test_and_set(&crc32_initialized, 1) != 0) {
        return;
    }
#endif

    /* 初始化 CRC 表 */
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (CRC32_POLY & ~(crc & 1));
        }
        crc32_table[i] = crc;
    }

    /* 写屏障：确保表初始化结果对其他线程可见 */
#if defined(_MSC_VER)
    _ReadWriteBarrier();
#else
    __sync_synchronize();
#endif
}

/* 计算 CRC32（独立计算，不支持增量更新）
 * crc 参数保留用于 API 兼容性，调用者应始终传 0 */
uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    (void)crc;  /* 当前实现不支持链式 CRC 计算 */
    if (!crc32_initialized) {
        crc32_init();
    }

    uint32_t c = CRC32_INIT;
    for (size_t i = 0; i < len; i++) {
        c = crc32_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ CRC32_INIT;
}

/* 转义编码：返回转义后数据（调用者需释放） */
uint8_t* msg_escape(const uint8_t *data, size_t len, size_t *out_len) {
    size_t escaped_len = len;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == 0x1F || data[i] == 0x1E || data[i] == 0x1C || data[i] == 0x1B) {
            escaped_len++;
        }
    }

    uint8_t *escaped = (uint8_t *)malloc(escaped_len);
    if (!escaped) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        switch (data[i]) {
            case 0x1F:
                escaped[j++] = 0x1B;
                escaped[j++] = MSG_ESC_CHAR_US;
                break;
            case 0x1E:
                escaped[j++] = 0x1B;
                escaped[j++] = MSG_ESC_CHAR_RS;
                break;
            case 0x1C:
                escaped[j++] = 0x1B;
                escaped[j++] = MSG_ESC_CHAR_FS;
                break;
            case 0x1B:
                escaped[j++] = 0x1B;
                escaped[j++] = MSG_ESC_CHAR_ESC;
                break;
            default:
                escaped[j++] = data[i];
                break;
        }
    }

    *out_len = escaped_len;
    return escaped;
}

/* 转义解码：返回解码后数据（调用者需释放） */
uint8_t* msg_unescape(const uint8_t *data, size_t len, size_t *out_len) {
    size_t unescaped_len = len;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == 0x1B && i + 1 < len) {
            unescaped_len--;
        }
    }

    uint8_t *unescaped = (uint8_t *)malloc(unescaped_len);
    if (!unescaped) return NULL;

    size_t j = 0;
    size_t i = 0;
    while (i < len) {
        if (data[i] == 0x1B) {
            if (i + 1 >= len) {
                free(unescaped);
                return NULL;  /* 孤立 ESC */
            }
            switch (data[i + 1]) {
                case MSG_ESC_CHAR_US:
                    unescaped[j++] = 0x1F;
                    break;
                case MSG_ESC_CHAR_RS:
                    unescaped[j++] = 0x1E;
                    break;
                case MSG_ESC_CHAR_FS:
                    unescaped[j++] = 0x1C;
                    break;
                case MSG_ESC_CHAR_ESC:
                    unescaped[j++] = 0x1B;
                    break;
                default:
                    free(unescaped);
                    return NULL;  /* 无效后缀 */
            }
            i += 2;
        } else {
            unescaped[j++] = data[i++];
        }
    }

    *out_len = unescaped_len;
    return unescaped;
}

/* 复制固定长度字段并补零 */
void msg_copy_fixed_field(char *dest, const char *src, size_t max_len) {
    memset(dest, 0, max_len);
    size_t len = 0;
    while (len < max_len && src[len]) len++;
    if (len) memcpy(dest, src, len);
}
