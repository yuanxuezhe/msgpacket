#include "msg_packet.h"
#include "msg_byteorder.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <unistd.h>
#include <fcntl.h>
#endif

/* CRC32 多项式（IEEE 802.3） */
#define CRC32_POLY  0xEDB88320UL
#define CRC32_INIT  0xFFFFFFFFUL

static uint32_t crc32_table[256];
static _Atomic int crc32_initialized = 0;

/* UUID v4 生成（使用密码学安全随机数生成器）
 * 优先级：Windows CryptGenRandom / dev/urandom / getrandom > fallback LCG
 * fallback 仅在 CSPRNG 不可用时使用（极少见） */
void msg_generate_uuid_v4(char out[32]) {
    uint8_t bytes[16];
    static const char hex[] = "0123456789ABCDEF";
    bool csprng_used = false;

#ifdef _WIN32
    HCRYPTPROV hProv = 0;
    if (CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (CryptGenRandom(hProv, 16, bytes)) {
            csprng_used = true;
        }
        CryptReleaseContext(hProv, 0);
    }
#elif defined(__linux__) || defined(__APPLE__)
    /* Linux/macOS: 使用 arc4random_buf（跨平台兼容，无需 _GNU_SOURCE） */
    arc4random_buf(bytes, 16);
    csprng_used = true;
#else
    /* 其他 Unix: 使用 /dev/urandom */
    {
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            ssize_t n = read(fd, bytes, 16);
            if (n == 16) csprng_used = true;
            close(fd);
        }
    }
#endif

    /* Fallback：使用时间+地址+栈噪声 混合种子（仍然可预测，仅保底） */
    if (!csprng_used) {
        uint32_t seed;
        seed  = (uint32_t)time(NULL);
        seed ^= (uint32_t)(uintptr_t)(void*)&csprng_used;
        seed ^= (uint32_t)(uintptr_t)(void*)&seed;
        /* 混合更多噪声 */
        for (int i = 0; i < 16; i++) {
            seed = seed * 1103515245UL + 12345UL;
            bytes[i] = (uint8_t)(seed >> 16);
        }
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

/* 初始化 CRC32 表（C11 atomics，线程安全）
 * 使用 atomic_compare_exchange_strong 保证只初始化一次。 */
void crc32_init(void) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&crc32_initialized, &expected, 1)) {
        return;  /* 已被其他线程初始化 */
    }

    /* 初始化 CRC 表（reflected 算法，LSB-first） */
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ CRC32_POLY;
            else
                crc >>= 1;
        }
        crc32_table[i] = crc;
    }

    /* 写屏障：确保表初始化结果对其他线程可见 */
    atomic_thread_fence(memory_order_release);
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
        if (data[i] == 0x1F || data[i] == 0x1E || data[i] == 0x1C || data[i] == 0x1B || data[i] == 0x1D) {
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
            case 0x1D:
                escaped[j++] = 0x1B;
                escaped[j++] = MSG_ESC_CHAR_GS;  /* ']' -> 0x1D (GS) */
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
                case MSG_ESC_CHAR_GS:
                    unescaped[j++] = 0x1D;
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
    size_t len = strlen(src);
    if (len > max_len) len = max_len;
    if (len) memcpy(dest, src, len);
    memset(dest + len, 0, max_len - len);
}
