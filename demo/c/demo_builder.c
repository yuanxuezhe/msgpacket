/**
 * demo_parser.c — MsgPacket 解包示例
 *
 * 演示如何解码收到的 MsgPacket 数据，验证 CRC，
 * 并以肉眼可读的字符形式输出消息内容（表格展示）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "msg_api.h"

/* ================================================================
 * 主入口
 * ================================================================ */
int main(void)
{
    printf("=== MsgPacket Parser Demo ===\n\n");

    /* ----------------------------------------------------------
     * 第一步：构建一个 Request 包（模拟收到客户端发来的数据）
     * ---------------------------------------------------------- */
    printf("--- Build a REQUEST packet (simulate client) ---\n");

    msg_packet_t *sender = msg_create(MSG_TYPE_REQUEST, "V1.0");
    if (!sender) { fprintf(stderr, "Failed\n"); return 1; }

    msg_set_func(sender, "subscribe");
    msg_set_code_int(sender, 1);
    msg_set_timestamp(sender, "20260501090101123");
     
    /*
    msg_set_headers(sender, 4, "Symbol,Price,Volume,Time");

    msg_begin_row(sender);
    msg_set_row(sender, "%s,%s,%.2f,%lld",
                "BTC/USDT", "65000.50", 1.2, (long long)1717000000000LL);
    msg_begin_row(sender);
    msg_set_row(sender, "%s,%s,%.2f,%lld",
                "ETH/USDT", "3500.00", 10.5, (long long)1717000000000LL);
    */

    msg_begin_row(sender);
    msg_set_value_str(sender, "col1", "123123");
    msg_set_value_str(sender, "col2", "65000.50");
    //msg_set_value_double(sender, "col3", 1.2);
    msg_set_value_i64(sender, "col4", 1717000000000LL);

    msg_finalize(sender);

    /* 获取 wire 格式数据 */
    const void *wire_data = msg_data(sender);
    size_t wire_size = msg_size(sender);

    char *sender_str = msg_wire_to_string(sender);

    printf(" Built packet: %s \n\n", sender_str);

    /* ----------------------------------------------------------
     * 第二步：解码收到的数据（模拟接收端）
     * ---------------------------------------------------------- */
    printf("--- Decode received data (simulate server) ---\n\n");

    msg_packet_t *received = NULL;
    int ret = msg_decode(wire_data, wire_size, &received);
    if (ret != 0) {
        printf("  Decode failed: error code %d\n", ret);

        const char *err_name = "UNKNOWN";
        switch (ret) {
        case MSG_ERR_INVALID_MAGIC:  err_name = "INVALID_MAGIC";  break;
        case MSG_ERR_CRC_MISMATCH:   err_name = "CRC_MISMATCH";   break;
        case MSG_ERR_BODY_TOO_LARGE: err_name = "BODY_TOO_LARGE"; break;
        }
        printf("  Error: %s\n", err_name);
        msg_destroy(sender);
        return 1;
    }

    char *received_str = msg_wire_to_string(received);
    printf(" Recved packet: %s \n\n", received_str);

    /* ----------------------------------------------------------
     * 第四步：遍历数据行
     * ---------------------------------------------------------- */
    printf("\n--- Step 4: Iterate Data Rows (key lookup) ---\n\n");

    while (msg_fetch_next(received)) {
        const char *sym = NULL, *price = NULL, *vol = NULL, *t = NULL;
        size_t sym_len, price_len, vol_len, t_len;

        msg_get_value_str(received, "col1", &sym, &sym_len);
        msg_get_value_str(received, "col2",  &price, &price_len);
        msg_get_value_str(received, "col3", &vol, &vol_len);
        msg_get_value_str(received, "col4",   &t, &t_len);

        printf("  Row %zu: %.*s | %.*s | %.*s | %.*s\n",
               msg_get_current_row(received),
               (int)sym_len,   sym   ? sym   : "?",
               (int)price_len, price ? price : "?",
               (int)vol_len,   vol   ? vol   : "?",
               (int)t_len,     t     ? t     : "?");
    }

    /* ----------------------------------------------------------
     * 第六步：按索引随机访问
     * ---------------------------------------------------------- */
    printf("\n--- Step 7: Random Access by Index ---\n\n");

    for (size_t row = 0; row < msg_get_row_count(received); row++) {
        printf("  Row %zu: ", row);
        for (size_t col = 0; col < msg_get_header_count(received); col++) {
            const char *val = NULL;
            size_t val_len = 0;
            msg_get_field(received, row, col, &val, &val_len);
            printf("%.*s", (int)val_len, val ? val : "");
            if (col < msg_get_header_count(received) - 1)
                printf(" | ");
        }
        printf("\n");
    }

    /* ----------------------------------------------------------
     * 第七步：构建并解码 Answer 包
     * ---------------------------------------------------------- */
    printf("\n--- Step 8: Parse ANSWER packet (simulate client receives response) ---\n\n");

    msg_packet_t *ans_builder = msg_create(MSG_TYPE_ANSWER, "V1.0");
    msg_set_func(ans_builder, "subscribe");
    msg_set_code(ans_builder, MSG_CODE_SUCCESS);
    msg_set_timestamp(ans_builder, NULL);
    msg_set_headers(ans_builder, 3, "Status,Message,ServerTime");

    msg_begin_row(ans_builder);
    msg_set_value_str(ans_builder, "Status", "OK");
    msg_set_value_str(ans_builder, "Message", "Subscribed successfully");
    msg_set_value_i64(ans_builder, "ServerTime", 1717000002000LL);

    msg_finalize(ans_builder);

    /* 解码 Answer */
    msg_packet_t *ans_parsed = NULL;
    ret = msg_decode(msg_data(ans_builder), msg_size(ans_builder), &ans_parsed);
    if (ret == 0) {
        msg_destroy(ans_parsed);
    }

    /* ----------------------------------------------------------
     * 清理
     * ---------------------------------------------------------- */
    msg_destroy(ans_builder);
    msg_destroy(received);
    msg_destroy(sender);

    printf("\n=== Parser demo completed successfully ===\n");
    return 0;
}
  