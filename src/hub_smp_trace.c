#include "hub_smp_decode.h"
#include "hub_classic_decode.h"
#include "hub_diag_ring.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "bluetooth.h"
#include "hci_dump.h"
#include "btstack_run_loop.h"
#include "btstack_util.h"

static hub_diag_ring_t history;

#if defined(__GNUC__)
static void trace_line(const char *format, ...) __attribute__((format(printf, 1, 2)));
#endif
static void trace_line(const char *format, ...) {
    char line[HUB_DIAG_LINE_BYTES];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    if (length < 0) {
        printf("[diag] message formatting failed\n");
        return;
    }
    if ((unsigned)length >= sizeof(line)) memcpy(line + sizeof(line) - 4u, "...", 4);
    size_t used = strlen(line);
    if (used && line[used - 1u] == '\n') line[used - 1u] = 0;
    if (!hub_diag_ring_push(&history, line)) {
        printf("[diag] cannot retain diagnostic line\n");
        return;
    }
    printf("%s\n", line);
}

void hub_pairing_note(const char *action, uint32_t ticket, int result) {
    trace_line("[APP %" PRIu32 "] %s ticket=%" PRIu32 " result=%d",
               btstack_run_loop_get_time_ms(), action, ticket, result);
}

void hub_pairing_diagnostics(void) {
    printf("[diag] recent %u events, oldest first; RAM only, cleared on reboot\n", history.count);
    for (unsigned i = 0; i < history.count; ++i) printf("%s\n", hub_diag_ring_get(&history, i));
    printf("[diag] end; timestamps are uptime milliseconds\n");
}

static void trace_classic(const hub_classic_metadata_t *metadata, const char *direction) {
    uint32_t now = btstack_run_loop_get_time_ms();
    switch (metadata->event) {
        case HUB_CLASSIC_PIN_REQUEST:
        case HUB_CLASSIC_KEY_REQUEST:
            trace_line("[PAIR %" PRIu32 "] %s %s peer=%s\n", now, direction,
                   metadata->event == HUB_CLASSIC_PIN_REQUEST ? "PIN-request" : "link-key-request",
                   bd_addr_to_str(metadata->address));
            break;
        case HUB_CLASSIC_PIN_REPLY:
        case HUB_CLASSIC_PIN_NEGATIVE:
            trace_line("[PAIR %" PRIu32 "] %s PIN-%s peer=%s length=%u (value hidden)\n",
                   now, direction, metadata->event == HUB_CLASSIC_PIN_REPLY ? "reply" : "negative",
                   bd_addr_to_str(metadata->address), metadata->value);
            break;
        case HUB_CLASSIC_PIN_ACK:
            trace_line("[PAIR %" PRIu32 "] %s PIN-command-ack opcode=0x%04x status=0x%02x\n",
                   now, direction, metadata->opcode, metadata->status);
            break;
        case HUB_CLASSIC_AUTH_COMPLETE:
            trace_line("[PAIR %" PRIu32 "] %s authentication-complete handle=0x%04x status=0x%02x\n",
                   now, direction, metadata->handle, metadata->status);
            break;
        case HUB_CLASSIC_ENCRYPTION:
            trace_line("[PAIR %" PRIu32 "] %s encryption-change handle=0x%04x status=0x%02x enabled=%u\n",
                   now, direction, metadata->handle, metadata->status, metadata->value);
            break;
        case HUB_CLASSIC_KEY_CREATED:
            trace_line("[PAIR %" PRIu32 "] %s link-key-created peer=%s type=%u (key hidden)\n",
                   now, direction, bd_addr_to_str(metadata->address), metadata->value);
            break;
        case HUB_CLASSIC_NAME:
            trace_line("[PAIR %" PRIu32 "] %s remote-name peer=%s status=0x%02x name=%s\n",
                   now, direction, bd_addr_to_str(metadata->address), metadata->status, metadata->name);
            break;
        case HUB_CLASSIC_DISCONNECTED:
            trace_line("[PAIR %" PRIu32 "] %s disconnected handle=0x%04x status=0x%02x reason=0x%02x\n",
                   now, direction, metadata->handle, metadata->status, metadata->value);
            break;
        case HUB_CLASSIC_AUTH_REQUEST:
        case HUB_CLASSIC_ENCRYPTION_REQUEST:
        case HUB_CLASSIC_DISCONNECT_REQUEST:
            trace_line("[PAIR %" PRIu32 "] %s %s handle=0x%04x value=0x%02x\n",
                       now, direction, metadata->event == HUB_CLASSIC_AUTH_REQUEST
                           ? "authentication-request"
                           : metadata->event == HUB_CLASSIC_ENCRYPTION_REQUEST
                               ? "encryption-request" : "disconnect-request",
                       metadata->handle, metadata->value);
            break;
        case HUB_CLASSIC_L2CAP_REQUEST:
            trace_line("[PAIR %" PRIu32 "] %s L2CAP-connect handle=0x%04x PSM=0x%04x source=0x%04x\n",
                       now, direction, metadata->handle, metadata->psm, metadata->source_cid);
            break;
        case HUB_CLASSIC_L2CAP_RESPONSE:
            trace_line("[PAIR %" PRIu32 "] %s L2CAP-response handle=0x%04x source=0x%04x "
                       "dest=0x%04x result=0x%04x status=0x%04x\n",
                       now, direction, metadata->handle, metadata->source_cid,
                       metadata->destination_cid, metadata->result, metadata->l2cap_status);
            break;
    }
}

static void trace_packet(uint8_t packet_type, uint8_t incoming, uint8_t *packet, uint16_t length) {
    hub_classic_metadata_t classic;
    if ((packet_type == HCI_COMMAND_DATA_PACKET && hub_classic_command_decode(packet, length, &classic))
        || (packet_type == HCI_EVENT_PACKET && hub_classic_event_decode(packet, length, &classic))) {
        trace_classic(&classic, incoming ? "RX" : "TX");
    }
    if (packet_type != HCI_ACL_DATA_PACKET) return;
    unsigned offset = 0;
    while (hub_classic_l2cap_next(packet, length, &offset, &classic)) {
        trace_classic(&classic, incoming ? "RX" : "TX");
    }
    hub_smp_metadata_t metadata;
    if (!hub_smp_decode(packet, length, &metadata)) return;
    const char *direction = incoming ? "RX" : "TX";
    if (metadata.code == 1 || metadata.code == 2) {
        trace_line("[SMP %" PRIu32 "] %s handle=0x%04x %s io=%u oob=%u auth=0x%02x max-key=%u "
               "key-distribution=0x%02x/0x%02x\n",
               btstack_run_loop_get_time_ms(), direction, metadata.handle, metadata.code == 1 ? "request" : "response",
               metadata.io, metadata.oob, metadata.auth, metadata.key_size,
               metadata.initiator_keys, metadata.responder_keys);
    } else if (metadata.code == 5) {
        trace_line("[SMP %" PRIu32 "] %s handle=0x%04x pairing-failed reason=0x%02x\n",
                   btstack_run_loop_get_time_ms(), direction, metadata.handle, metadata.reason);
    } else {
        trace_line("[SMP %" PRIu32 "] %s handle=0x%04x security-request auth=0x%02x\n",
                   btstack_run_loop_get_time_ms(), direction, metadata.handle, metadata.auth);
    }
}

static void trace_message(int level, const char *format, va_list arguments) {
    if (level != HCI_DUMP_LOG_LEVEL_ERROR) return;
    printf("[BTstack error] ");
    vprintf(format, arguments);
    printf("\n");
}

void hub_smp_trace_init(void) {
    hub_diag_ring_init(&history);
    /* Never dump raw HCI packets: key distribution and HID payloads are private. */
    static const hci_dump_t trace = {
        .reset = NULL,
        .log_packet = trace_packet,
        .log_message = trace_message,
    };
    hci_dump_init(&trace);
    hci_dump_enable_log_level(HCI_DUMP_LOG_LEVEL_DEBUG, 0);
    hci_dump_enable_log_level(HCI_DUMP_LOG_LEVEL_INFO, 0);
}
