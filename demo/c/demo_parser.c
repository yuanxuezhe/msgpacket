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
 * 辅助：可读形式打印 Body（原始格式 + 标记）
 * ================================================================ */
static void print_body_raw(const msg_packet_t *packet)
{
    const uint8_t *data = (const uint8_t *)msg_data(packet);
    if (!data) return;
    const uint8_t *body = data + BODY_OFFSET;
    uint32_t body_len = msg_get_body_len(packet);

    printf("  Body raw (%u bytes): ", body_len);
    for (uint32_t i = 0; i < body_len; i++) {
        switch (body[i]) {
        case 0x1F: printf("<US>"); break;
        case 0x1E: printf("<RS>"); break;
        case 0x1C: printf("<FS>"); break;
        case 0x1B: printf("<ESC>"); break;
        default:   putchar(isprint(body[i]) ? body[i] : '#'); break;
        }
    }
    printf("\n");
}

/* ================================================================
 * 辅助：以表格形式打印数据
 * ================================================================ */
static void print_table(msg_packet_t *packet)
{
    size_t hdr_count = msg_get_header_count(packet);
    if (hdr_count == 0) { printf("  (no headers)\n"); return; }

    /* 第一遍：获取表头名称 */
    char hdr_buf[4096] = {0};
    size_t hdr_len = sizeof(hdr_buf);
    msg_get_headers(packet, hdr_buf, &hdr_len);

    char *header_names[256];
    size_t col_w[256];
    size_t actual_hdrs = 0;

    char *p = hdr_buf;
    for (size_t i = 0; i < hdr_count && *p; i++) {
        while (*p == ' ') p++;
        header_names[i] = p;
        char *end = strchr(p, ',');
        if (end) { *end = '\0'; p = end + 1; }
        else p += strlen(p);
        col_w[i] = strlen(header_names[i]);
        actual_hdrs++;
    }

    /* 第二遍：扫描数据计算列宽 */
    msg_reset_cursor(packet);
    while (msg_fetch_next(packet)) {
        for (size_t col = 0; col < actual_hdrs; col++) {
            const char *val = NULL;
            size_t val_len = 0;
            if (msg_get_field(packet, msg_get_current_row(packet),
                              col, &val, &val_len) == 0 && val) {
                if (val_len > col_w[col]) col_w[col] = val_len;
            }
        }
    }
    msg_reset_cursor(packet);

    /* 打印表头 */
    printf("  | ");
    for (size_t i = 0; i < actual_hdrs; i++) {
        printf("%-*s", (int)col_w[i], header_names[i]);
        if (i < actual_hdrs - 1) printf(" | ");
    }
    printf(" |\n");

    /* 分隔线 */
    printf("  |-");
    for (size_t i = 0; i < actual_hdrs; i++) {
        for (size_t j = 0; j < col_w[i]; j++) putchar('-');
        if (i < actual_hdrs - 1) printf("-+-");
    }
    printf("-|\n");

    /* 打印数据行 */
    while (msg_fetch_next(packet)) {
        printf("  | ");
        for (size_t col = 0; col < actual_hdrs; col++) {
            const char *val = NULL;
            size_t val_len = 0;
            if (msg_get_field(packet, msg_get_current_row(packet),
                              col, &val, &val_len) == 0 && val) {
                printf("%-*.*s", (int)col_w[col], (int)val_len, val);
            } else {
                printf("%-*s", (int)col_w[col], "");
            }
            if (col < actual_hdrs - 1) printf(" | ");
        }
        printf(" |\n");
    }
    msg_reset_cursor(packet);
}

/* ================================================================
 * 辅助：打印 Header
 * ================================================================ */
static void print_header(const msg_packet_t *packet)
{
    const char *tname = "UNKNOWN";
    switch (msg_get_type(packet)) {
    case MSG_TYPE_REQUEST:   tname = "REQUEST";   break;
    case MSG_TYPE_ANSWER:    tname = "ANSWER";    break;
    case MSG_TYPE_PUSH:      tname = "PUSH";      break;
    case MSG_TYPE_HEARTBEAT: tname = "HEARTBEAT"; break;
    }

    printf("  msg_id:    %.32s\n",  msg_get_msg_id(packet));
    printf("  version:   %.8s\n",   msg_get_version(packet));
    printf("  func:      %.8s\n",   msg_get_func(packet));
    printf("  type:      %s (%c)\n", tname, (char)msg_get_type(packet));
    printf("  format:    %u '%c'\n", msg_get_format(packet),
           (char)msg_get_format(packet));
    {
        const char *ts = msg_get_timestamp(packet);
        printf("  timestamp: %.17s (%.4s-%.2s-%.2s %.2s:%.2s:%.2s.%.3s)\n",
               ts ? ts : "",
               ts ? ts : "", ts ? ts + 4 : "", ts ? ts + 6 : "",
               ts ? ts + 8 : "", ts ? ts + 10 : "", ts ? ts + 12 : "",
               ts ? ts + 14 : "");
    }
    printf("  body_len:  %u bytes\n", msg_get_body_len(packet));
    printf("  row_count: %zu\n",      msg_get_row_count(packet));
}

/* ================================================================
 * 主入口
 * ================================================================ */
int main(void)
{
    printf("=== MsgPacket Parser Demo ===\n\n");

    /* ----------------------------------------------------------
     * 第一步：构建一个 Request 包（模拟收到客户端发来的数据）
     * ---------------------------------------------------------- */
    printf("--- Step 1: Build a REQUEST packet (simulate client) ---\n");

    msg_packet_t *sender = msg_create(MSG_TYPE_REQUEST, "V1.0");
    if (!sender) { fprintf(stderr, "Failed\n"); return 1; }

    msg_set_func(sender, "subscribe");
    msg_set_timestamp(sender, NULL);
    msg_set_headers(sender, 4, "Symbol,Price,Volume,Time");

    msg_add_row(sender);
    msg_set_row(sender, "%s,%s,%.2f,%lld",
                "BTC/USDT", "65000.50", 1.2, (long long)1717000000000LL);
    msg_add_row(sender);
    msg_set_row(sender, "%s,%s,%.2f,%lld",
                "ETH/USDT", "3500.00", 10.5, (long long)1717000000000LL);

    msg_finalize(sender);

    /* 获取 wire 格式数据 */
    const void *wire_data = msg_data(sender);
    size_t wire_size = msg_size(sender);

    printf("  ✓ Built packet: %zu bytes total\n\n", wire_size);

    /* ----------------------------------------------------------
     * 第二步：解码收到的数据（模拟接收端）
     * ---------------------------------------------------------- */
    printf("--- Step 2: Decode received data (simulate server) ---\n\n");

    msg_packet_t *received = NULL;
    int ret = msg_decode(wire_data, wire_size, &received);
    if (ret != 0) {
        printf("  ✗ Decode failed: error code %d\n", ret);

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

    printf("  ✓ Magic: '%.4s' — OK\n", (const char *)wire_data);
    printf("  ✓ CRC: verified\n\n");

    /* ----------------------------------------------------------
     * 第三步：读取并展示 Header
     * ---------------------------------------------------------- */
    printf("--- Step 3: Read Header ---\n\n");
    print_header(received);

    /* ----------------------------------------------------------
     * 第四步：遍历数据行
     * ---------------------------------------------------------- */
    printf("\n--- Step 4: Iterate Data Rows (key lookup) ---\n\n");

    while (msg_fetch_next(received)) {
        const char *sym = NULL, *price = NULL, *vol = NULL, *t = NULL;
        size_t sym_len, price_len, vol_len, t_len;

        msg_get_value_str(received, "Symbol", &sym, &sym_len);
        msg_get_value_str(received, "Price",  &price, &price_len);
        msg_get_value_str(received, "Volume", &vol, &vol_len);
        msg_get_value_str(received, "Time",   &t, &t_len);

        printf("  Row %zu: %.*s | %.*s | %.*s | %.*s\n",
               msg_get_current_row(received),
               (int)sym_len,   sym   ? sym   : "?",
               (int)price_len, price ? price : "?",
               (int)vol_len,   vol   ? vol   : "?",
               (int)t_len,     t     ? t     : "?");
    }

    /* ----------------------------------------------------------
     * 第五步：表格视图 + 原始 Body
     * ---------------------------------------------------------- */
    printf("\n--- Step 5: Table View ---\n\n");
    print_table(received);

    printf("\n--- Step 6: Raw Body with Markers ---\n\n");
    print_body_raw(received);

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
    msg_set_timestamp(ans_builder, NULL);
    msg_set_headers(ans_builder, 3, "Status,Message,ServerTime");

    msg_add_row(ans_builder);
    msg_set_value_str(ans_builder, "Status", "OK");
    msg_set_value_str(ans_builder, "Message", "Subscribed successfully");
    msg_set_value_i64(ans_builder, "ServerTime", 1717000002000LL);

    msg_finalize(ans_builder);

    /* 解码 Answer */
    msg_packet_t *ans_parsed = NULL;
    ret = msg_decode(msg_data(ans_builder), msg_size(ans_builder), &ans_parsed);
    if (ret == 0) {
        printf("  ✓ Answer decoded OK\n\n");
        print_header(ans_parsed);

        printf("\n  Answer Table:\n");
        print_table(ans_parsed);

        printf("\n  Raw Body:\n");
        print_body_raw(ans_parsed);

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
