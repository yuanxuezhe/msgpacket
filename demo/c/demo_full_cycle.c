/**
 * demo_full_cycle.c — Complete MsgPacket request-response packing/unpacking example
 *
 * Flow:
 *   [Client] Build Request -> Encode -> Send
 *   [Server] Decode Request -> Build Answer -> Encode -> Send
 *   [Client] Decode Answer -> Display result
 *
 * All output is human-readable, no hex dumps.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "msg_api.h"

/* ================================================================
 * Helper: Print body content in readable format
 * ================================================================ */
static void print_body_readable(const uint8_t *body, size_t body_len)
{
    printf("  Body raw (%zu bytes): ", body_len);
    for (size_t i = 0; i < body_len; i++) {
        uint8_t c = body[i];
        switch (c) {
        case 0x1F: printf("<US>");   break;
        case 0x1E: printf("<RS>");   break;
        case 0x1C: printf("<FS>");   break;
        case 0x1B: printf("<ESC>");  break;
        default:   putchar(isprint(c) ? c : '#'); break;
        }
    }
    printf("\n");
}

/* ================================================================
 * Helper: Extract and print body data from finalized packet
 * ================================================================ */
static void print_packet_body(const msg_packet_t *packet)
{
    const uint8_t *data = (const uint8_t *)msg_data(packet);
    size_t size = msg_size(packet);
    if (!data || size == 0) {
        printf("  (no data)\n");
        return;
    }
    const uint8_t *body = data + BODY_OFFSET;
    size_t body_len = msg_get_body_len(packet);
    if (body_len > 0)
        print_body_readable(body, body_len);
    else
        printf("  (empty body)\n");
}

/* ================================================================
 * Helper: Print entire wire data as readable string
 * Convert via msg_wire_to_string, then print directly
 * ================================================================ */
static void print_entire_wire(const msg_packet_t *packet)
{
    char *str = msg_wire_to_string(packet);
    if (str) {
        printf("  ---- Wire data (%zu bytes, from msg_id) ----\n  %s\n",
               msg_size(packet) - 12, str);
        msg_free_buffer(str);
    } else {
        printf("  (no data)\n");
    }
}

/* ================================================================
 * Helper: Print Header info
 * ================================================================ */
static void print_header(const msg_packet_t *packet)
{
    char msg_type_char = (char)msg_get_type(packet);
    const char *type_name;
    switch (msg_get_type(packet)) {
    case MSG_TYPE_REQUEST:   type_name = "REQUEST";   break;
    case MSG_TYPE_ANSWER:    type_name = "ANSWER";    break;
    case MSG_TYPE_PUSH:      type_name = "PUSH";      break;
    case MSG_TYPE_HEARTBEAT: type_name = "HEARTBEAT"; break;
    default:                 type_name = "UNKNOWN";    break;
    }

    printf("  msg_id:    %.32s\n", msg_get_msg_id(packet));
    printf("  version:   %.8s\n",  msg_get_version(packet));
    printf("  func:      %.8s\n",  msg_get_func(packet));
    printf("  type:      %s (0x%02X '%c')\n", type_name,
           msg_get_type(packet), msg_type_char);
    printf("  format:    %u '%c'\n", msg_get_format(packet),
           (char)msg_get_format(packet));
    printf("  code:      %.5s\n", msg_get_code(packet));

    /* timestamp: yyyyMMddHHmmssSSS (17 chars) */
    {
        const char *ts = msg_get_timestamp(packet);
        printf("  timestamp: %.17s (%.4s-%.2s-%.2s %.2s:%.2s:%.2s.%.3s)\n",
               ts ? ts : "",
               ts ? ts : "", ts ? ts + 4 : "", ts ? ts + 6 : "",
               ts ? ts + 8 : "", ts ? ts + 10 : "", ts ? ts + 12 : "",
               ts ? ts + 14 : "");
    }
    printf("  body_len:  %u bytes\n", msg_get_body_len(packet));
    printf("  total_len: %zu bytes\n", msg_get_total_len(packet));
}

/* ================================================================
 * Helper: Local decode (for builder-side table view display)
 * After msg_finalize, internal parsing state is cleared, need decode for cursor traversal
 * ================================================================ */
static msg_packet_t* local_decode_for_display(msg_packet_t *packet)
{
    void *buf = NULL;
    size_t len = 0;
    if (msg_encode(packet, &buf, &len) != 0) return NULL;
    msg_packet_t *decoded = NULL;
    msg_decode(buf, len, &decoded);
    msg_free_buffer(buf);
    return decoded;
}

/* ================================================================
 * Helper: Print body data in table format
 * ================================================================ */
static void print_table_view(msg_packet_t *packet)
{
    size_t hdr_count = msg_get_header_count(packet);
    size_t row_count = msg_get_row_count(packet);

    if (hdr_count == 0) {
        printf("  (no headers)\n");
        return;
    }

    /* Step 1: Collect all column widths */
    size_t *col_widths = (size_t *)calloc(hdr_count, sizeof(size_t));
    if (!col_widths) return;

    /* Get header names and calculate initial column widths */
    char hdr_buf[4096] = {0};
    size_t hdr_buf_len = sizeof(hdr_buf);
    msg_get_headers(packet, hdr_buf, &hdr_buf_len);

    /* Parse header names (manual split for better compatibility) */
    char **header_names = (char **)calloc(hdr_count, sizeof(char *));
    if (!header_names) { free(col_widths); return; }

    char *p = hdr_buf;
    for (size_t i = 0; i < hdr_count; i++) {
        while (*p == ' ' || *p == '\t') p++;
        header_names[i] = p;
        char *end = strchr(p, ',');
        if (end) {
            *end = '\0';
            p = end + 1;
        } else {
            p = p + strlen(p);
        }
        col_widths[i] = strlen(header_names[i]);
    }

    /* Scan all row data and update column widths */
    msg_reset_cursor(packet);
    while (msg_fetch_next(packet)) {
        for (size_t col = 0; col < hdr_count; col++) {
            const char *val = NULL;
            size_t val_len = 0;
            if (msg_get_field(packet, msg_get_current_row(packet), col,
                              &val, &val_len) == 0 && val) {
                if (val_len > col_widths[col])
                    col_widths[col] = val_len;
            }
        }
    }
    msg_reset_cursor(packet);

    /* Step 2: Print header */
    printf("  | ");
    for (size_t i = 0; i < hdr_count; i++) {
        printf("%-*s", (int)col_widths[i], header_names[i] ? header_names[i] : "");
        if (i < hdr_count - 1) printf(" | ");
    }
    printf(" |\n");

    /* Separator line */
    printf("  |-");
    for (size_t i = 0; i < hdr_count; i++) {
        for (size_t j = 0; j < col_widths[i]; j++) putchar('-');
        if (i < hdr_count - 1) printf("-+-");
    }
    printf("-|\n");

    /* Step 3: Print data rows */
    if (row_count == 0) {
        printf("  (no data rows)\n");
    } else {
        while (msg_fetch_next(packet)) {
            printf("  | ");
            for (size_t col = 0; col < hdr_count; col++) {
                const char *val = NULL;
                size_t val_len = 0;
                if (msg_get_field(packet, msg_get_current_row(packet), col,
                                  &val, &val_len) == 0 && val) {
                    printf("%-*.*s", (int)col_widths[col],
                           (int)val_len, val);
                } else {
                    printf("%-*s", (int)col_widths[col], "");
                }
                if (col < hdr_count - 1) printf(" | ");
            }
            printf(" |\n");
        }
    }
    msg_reset_cursor(packet);

    free(header_names);
    free(col_widths);
}

/* ================================================================
 * Build request packet
 * ================================================================ */
static msg_packet_t* build_request_packet(void)
{
    msg_packet_t *req = msg_create(MSG_TYPE_REQUEST, "V1.0");
    if (!req) {
        fprintf(stderr, "Failed to create request packet\n");
        return NULL;
    }

    msg_set_func(req, "getData");
    msg_set_code(req, MSG_CODE_SUCCESS);
    msg_set_timestamp(req, NULL);

    /* Set headers */
    msg_set_headers(req, 4, "Symbol,Price,Volume,Time");

    /* Add data rows (request parameters) */
    msg_begin_row(req);
    msg_set_row(req, "%s,%s,%.2f,%lld",
                "BTC/USDT", "65000.5", 1.2, (long long)1717000000000LL);

    msg_begin_row(req);
    msg_set_row(req, "%s,%s,%.2f,%lld",
                "ETH/USDT", "3500.0", 10.5, (long long)1717000000000LL);

    msg_begin_row(req);
    msg_set_row(req, "%s,%s,%.2f,%lld",
                "SOL/USDT", "150.0", 100.0, (long long)1717000000000LL);

    /* Finalize and pack */
    int ret = msg_finalize(req);
    if (ret != 0) {
        fprintf(stderr, "Finalize request failed: %d\n", ret);
        msg_destroy(req);
        return NULL;
    }

    return req;
}

/* ================================================================
 * Build answer packet
 * ================================================================ */
static msg_packet_t* build_answer_packet(void)
{
    msg_packet_t *ans = msg_create(MSG_TYPE_ANSWER, "V1.0");
    if (!ans) {
        fprintf(stderr, "Failed to create answer packet\n");
        return NULL;
    }

    msg_set_func(ans, "getData");
    msg_set_code(ans, MSG_CODE_SUCCESS);
    msg_set_timestamp(ans, NULL);

    /* Set headers */
    msg_set_headers(ans, 4, "Symbol,Price,Volume,Time");

    /* Add answer data rows */
    msg_begin_row(ans);
    msg_set_row(ans, "%s,%s,%.2f,%lld",
                "BTC/USDT", "65500.0", 2.5, (long long)1717000001000LL);

    msg_begin_row(ans);
    msg_set_row(ans, "%s,%s,%.2f,%lld",
                "ETH/USDT", "3520.0", 15.8, (long long)1717000001000LL);

    msg_begin_row(ans);
    msg_set_row(ans, "%s,%s,%.2f,%lld",
                "SOL/USDT", "152.0", 200.0, (long long)1717000001000LL);

    /* Finalize and pack */
    int ret = msg_finalize(ans);
    if (ret != 0) {
        fprintf(stderr, "Finalize answer failed: %d\n", ret);
        msg_destroy(ans);
        return NULL;
    }

    return ans;
}

/* ================================================================
 * Simulate network transfer: Encode -> Decode
 * ================================================================ */
static msg_packet_t* simulate_transfer(msg_packet_t *packet)
{
    void *wire_buf = NULL;
    size_t wire_len = 0;

    int ret = msg_encode(packet, &wire_buf, &wire_len);
    if (ret != 0) {
        fprintf(stderr, "Encode failed: %d\n", ret);
        return NULL;
    }

    printf("  Wire size: %zu bytes\n", wire_len);

    msg_packet_t *decoded = NULL;
    ret = msg_decode(wire_buf, wire_len, &decoded);
    if (ret != 0) {
        fprintf(stderr, "Decode failed: %d\n", ret);
        msg_free_buffer(wire_buf);
        return NULL;
    }

    msg_free_buffer(wire_buf);
    return decoded;
}

/* ================================================================
 * Main entry
 * ================================================================ */
int main(void)
{
    printf("     MsgPacket Full Cycle Demo -- req & ans      ||\n");

    /* ============================================================
     * Phase 1: Client builds Request packet
     * ============================================================ */
    printf("\n [Phase 1] Client: Build Request Packet\n");

    msg_packet_t *request = build_request_packet();
    if (!request) return 1;

    printf("  Header:\n");
    print_header(request);

    printf("\nEntire Wire Data (complete message)\n");
    print_entire_wire(request);


    printf("  Body (raw with markers):\n");
    print_packet_body(request);

    printf("  Body (table view):\n");
    {
        msg_packet_t *display = local_decode_for_display(request);
        if (display) {
            print_table_view(display);
            msg_destroy(display);
        }
    }

    printf("\n");

    /* ============================================================
     * Phase 2: Simulate Client -> Server transfer
     * ============================================================ */
    printf("\n[Phase 2] Network Transfer: Client -> Server\n");

    msg_packet_t *received_req = simulate_transfer(request);
    if (!received_req) { msg_destroy(request); return 1; }

    printf("  Magic check:  %.4s  OK\n",
           (const char *)msg_data(received_req));
    printf("  CRC verified: OK\n");
    printf("  Transfer OK\n");

    /* ============================================================
     * Phase 3: Server decodes Request and builds Answer
     * ============================================================ */
    printf("\n[Phase 3] Server: Parse Request & Build Answer\n");

    printf("  Received Request:\n");
    printf("  func: [%.8s]  type: [%c]  code: [%.5s]\n",
           msg_get_func(received_req),
           (char)msg_get_type(received_req),
           msg_get_code(received_req));

    printf("\n");
    printf("  Processing request...\n");
    printf("  Looking up market data for requested symbols...\n");

    /* Iterate through symbols in request */
    while (msg_fetch_next(received_req)) {
        const char *sym = NULL;
        size_t sym_len = 0;
        msg_get_value_str(received_req, "Symbol", &sym, &sym_len);
        printf("    Query: %.*s\n", (int)sym_len, sym ? sym : "?");
    }

    /* Build answer packet */
    msg_packet_t *answer = build_answer_packet();
    if (!answer) {
        msg_destroy(received_req);
        msg_destroy(request);
        return 1;
    }

    printf("\n");
    printf("  Built Answer Packet:\n");
    print_header(answer);

    printf("\n");
    printf("  Answer Body (raw with markers):\n");
    print_packet_body(answer);

    printf("\n");
    printf("  === Entire Wire Data (answer packet) ===\n");
    print_entire_wire(answer);

    printf("\n");
    printf("  Answer Body (table view):\n");
    {
        msg_packet_t *display = local_decode_for_display(answer);
        if (display) {
            print_table_view(display);
            msg_destroy(display);
        }
    }

    /* ============================================================
     * Phase 4: Simulate Server -> Client transfer
     * ============================================================ */
    printf("\n[Phase 4] Network Transfer: Server -> Client\n");

    msg_packet_t *received_ans = simulate_transfer(answer);
    if (!received_ans) {
        msg_destroy(answer);
        msg_destroy(received_req);
        msg_destroy(request);
        return 1;
    }

    printf("  Magic check:  %.4s  OK\n",
           (const char *)msg_data(received_ans));
    printf("  CRC verified: OK\n");
    printf("  Transfer OK\n");

    /* ============================================================
     * Phase 5: Client decodes Answer
     * ============================================================ */
    printf("\n[Phase 5] Client: Parse Answer\n");

    printf("  Received Answer:\n");
    printf("  func: [%.8s]  type: [%c]  code: [%.5s]\n",
           msg_get_func(received_ans),
           (char)msg_get_type(received_ans),
           msg_get_code(received_ans));

    printf("\n");
    printf("  Answer Data (table view):\n");
    print_table_view(received_ans);

    printf("\n");
    printf("  Answer Body (raw with marker):\n");
    print_packet_body(received_ans);

    /* ============================================================
     * Cleanup
     * ============================================================ */
    msg_destroy(received_ans);
    msg_destroy(answer);
    msg_destroy(received_req);
    msg_destroy(request);

    printf("\n============================================================\n");
    printf("           Demo completed successfully!\n");
    printf("============================================================\n");

    return 0;
}