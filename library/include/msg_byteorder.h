#ifndef MSG_BYTEORDER_H
#define MSG_BYTEORDER_H

#include <stdint.h>

/* 字节序检测和转换宏（统一使用小端序） */
#if defined(__BIG_ENDIAN__) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    /* Big-Endian 平台 */
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
        static inline uint64_t MSG_HTOLE64(uint64_t x) {
            return (((x) & 0xFF00000000000000ULL) >> 56) |
                   (((x) & 0x00FF000000000000ULL) >> 40) |
                   (((x) & 0x0000FF0000000000ULL) >> 24) |
                   (((x) & 0x000000FF00000000ULL) >> 8)  |
                   (((x) & 0x00000000FF000000ULL) << 8)  |
                   (((x) & 0x0000000000FF0000ULL) << 24) |
                   (((x) & 0x000000000000FF00ULL) << 40) |
                   (((x) & 0x00000000000000FFULL) << 56);
        }
        static inline uint32_t MSG_HTOLE32(uint32_t x) {
            return (((x) & 0xFF000000UL) >> 24) |
                   (((x) & 0x00FF0000UL) >> 8)  |
                   (((x) & 0x0000FF00UL) << 8)  |
                   (((x) & 0x000000FFUL) << 24);
        }
        #define MSG_LE64TOH(x)   MSG_HTOLE64(x)
        #define MSG_LE32TOH(x)   MSG_HTOLE32(x)
    #endif
#else
    /* Little-Endian 平台（x86/x64/ARM LE 等） */
    #define MSG_IS_BIG_ENDIAN    0
    #define MSG_HTOLE64(x)       (x)
    #define MSG_HTOLE32(x)       (x)
    #define MSG_LE64TOH(x)       (x)
    #define MSG_LE32TOH(x)       (x)
#endif

#endif /* MSG_BYTEORDER_H */
