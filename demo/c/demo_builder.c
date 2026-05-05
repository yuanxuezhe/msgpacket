/**
 * demo_builder.c — MsgPacket 多结果集打包示例
 *
 * 演示如何构建含多个结果集的包，并解码验证。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "msg_api.h"

/* ================================================================
 * 辅助：打印结果集信息
 * ================================================================ */
static void print_result_set_info(msg_packet_t *pkt, size_t rs_number)
{
    printf("  Result Set %zu:\n", rs_number);
    printf("    headers: %zu, rows: %zu\n",
           msg_get_header_count(pkt), msg_get_row_count(pkt));

    /* 打印表头 */
    char hdr_buf[4096] = {0};
    size_t hdr_len = sizeof(hdr_buf);
    msg_get_headers(pkt, hdr_buf, &hdr_len);
    printf("    header: %s\n", hdr_buf);

    /* 打印数据 */
    msg_reset_cursor(pkt);
    while (msg_fetch_next(pkt)) {
        size_t row = msg_get_current_row(pkt);
        printf("    row %zu:", row);
        for (size_t col = 0; col < msg_get_header_count(pkt); col++) {
            const char *val = NULL;
            size_t val_len = 0;
            msg_get_field(pkt, row, col, &val, &val_len);
            printf(" %.*s", (int)val_len, val ? val : "");
        }
        printf("\n");
    }
    msg_reset_cursor(pkt);
}

/* ================================================================
 * 辅助：解码并遍历所有结果集
 * ================================================================ */
static void decode_and_print_all_rs(const void *wire_data, size_t wire_size)
{
    msg_packet_t *pkt = NULL;
    int ret = msg_decode(wire_data, wire_size, &pkt);
    if (ret != 0) {
        printf("  Decode failed: %d\n", ret);
        return;
    }

    printf("  === Decoded Packet ===\n");
    printf("  func: %.8s  type: %c\n",
           msg_get_func(pkt), msg_get_type(pkt));
    printf("  result_set_count: %zu\n", msg_get_result_set_count(pkt));

    /* 遍历所有结果集，使用 msg_next_result_set 切换 */
    printf("  === Iterate all result sets ===\n");
    for (size_t rs = 1; rs <= msg_get_result_set_count(pkt); rs++) {
        if (rs > 1) {
            if (!msg_next_result_set(pkt)) {
                printf("  Failed to switch to RS%zu\n", rs);
                break;
            }
        }
        printf("  [RS%zu] current_rs=%zu, rs_count=%zu, row_count=%zu\n",
               rs, msg_get_result_set(pkt),
               msg_get_result_set_count(pkt), msg_get_row_count(pkt));
        print_result_set_info(pkt, rs);
    }

    msg_destroy(pkt);
}

/* ================================================================
 * 主入口
 * ================================================================ */
int main(void)
{
    printf("=== MsgPacket Multi-Result-Set Builder Demo ===\n\n");

    /* ----------------------------------------------------------
     * 构建含多个结果集的包
     * ---------------------------------------------------------- */
    printf("--- Build multi result-set packet ---\n");

    msg_packet_t *sender = msg_create(MSG_TYPE_REQUEST, "V1.0");
    if (!sender) { fprintf(stderr, "Failed to create\n"); return 1; }

    msg_set_func(sender, "subscribe");
    msg_set_timestamp(sender, "20260501090101123");

    /* RS1: 请求参数 */
    printf("\n  Building RS1...\n");
    msg_set_headers(sender, 2, "Symbol,Price");
    msg_add_row(sender);
    msg_set_row(sender, "%s,%s", "BTC/USDT", "65000.50");

    msg_add_row(sender);
    msg_set_row(sender, "%s,%s", "ETH/USDT", "3500.00");

    /* RS2: 附加信息 */
    printf("  Adding RS2...\n");
    msg_add_result_set(sender);
    msg_set_headers(sender, 2, "Tag,Note");
    //msg_add_row(sender);
    int rc2 = msg_set_row(sender, "%s,%s", "priority", "high-frequency");
    printf("  msg_set_row for RS2 returned: %d\n", rc2);

    /* RS3: 扩展字段 */
    printf("  Adding RS3...\n");
    msg_add_result_set(sender);
    msg_set_headers(sender, 2, "Ext1,Ext2");
    msg_add_row(sender);
    msg_add_row(sender);
    msg_add_row(sender);
    msg_set_row(sender, "%s,%s", "ext_value_1", "ext_value_2");

    msg_finalize(sender);

    /* 获取 wire 格式数据 */
    const void *wire_data = msg_data(sender);
    size_t wire_size = msg_size(sender);

    char *sender_str = msg_wire_to_string(sender);
    printf("\n  Wire data (%zu bytes):\n  %s\n", wire_size, sender_str);
    msg_free_buffer(sender_str);

    /* ----------------------------------------------------------
     * 解码验证
     * ---------------------------------------------------------- */
    printf("\n--- Decode and verify ---\n");
    decode_and_print_all_rs(wire_data, wire_size);

    /* ----------------------------------------------------------
     * 清理
     * ---------------------------------------------------------- */
    msg_destroy(sender);

    printf("\n=== Demo completed successfully ===\n");
    return 0;
}