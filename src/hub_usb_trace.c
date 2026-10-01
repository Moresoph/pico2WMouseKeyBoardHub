#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/sync.h"
#include "hub_usb_trace.h"

#define USB_TRACE_BYTES 8192u
#define USB_TRACE_CHUNK 64u

static char trace[USB_TRACE_BYTES], snapshot[USB_TRACE_BYTES];
static unsigned next, used, snapshot_size, snapshot_offset;
static uint32_t overwritten, truncated, format_errors;

int hub_usb_trace_printf(const char *format, ...) {
    char text[192];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    uint32_t irq = save_and_disable_interrupts();
    if (length < 0) {
        if (format_errors != UINT32_MAX) ++format_errors;
    } else {
        unsigned count = (unsigned)length;
        if (count >= sizeof(text)) {
            count = sizeof(text) - 1u;
            if (truncated != UINT32_MAX) ++truncated;
        }
        for (unsigned i = 0; i < count; ++i) {
            trace[next] = text[i];
            next = (next + 1u) % USB_TRACE_BYTES;
            if (used < USB_TRACE_BYTES) ++used;
            else if (overwritten != UINT32_MAX) ++overwritten;
        }
    }
    restore_interrupts(irq);
    return length;
}

void hub_usb_trace_status(void) {
    uint32_t irq = save_and_disable_interrupts();
    unsigned bytes = used;
    uint32_t lost = overwritten, cuts = truncated, errors = format_errors;
    restore_interrupts(irq);
    printf("[usb-trace] retained=%u overwritten-bytes=%" PRIu32
           " truncated-calls=%" PRIu32 " format-errors=%" PRIu32
           " replay=%u; use usb trace\n",
           bytes, lost, cuts, errors, snapshot_offset < snapshot_size);
}

void hub_usb_trace_dump(void) {
    if (snapshot_offset < snapshot_size) {
        printf("[usb-trace] replay already in progress\n");
        return;
    }
    uint32_t irq = save_and_disable_interrupts();
    snapshot_size = used;
    snapshot_offset = 0;
    unsigned first = (next + USB_TRACE_BYTES - used) % USB_TRACE_BYTES;
    unsigned head = USB_TRACE_BYTES - first;
    if (head > used) head = used;
    memcpy(snapshot, trace + first, head);
    memcpy(snapshot + head, trace, used - head);
    restore_interrupts(irq);
    hub_usb_trace_status();
    printf("[usb-trace] BEGIN snapshot bytes=%u; replay is paced, capture continues\n", snapshot_size);
    if (!snapshot_size) printf("[usb-trace] END (empty)\n");
}

void hub_usb_trace_poll(void) {
    if (snapshot_offset == snapshot_size) return;
    unsigned count = snapshot_size - snapshot_offset;
    if (count > USB_TRACE_CHUNK) count = USB_TRACE_CHUNK;
    size_t written = fwrite(snapshot + snapshot_offset, 1, count, stdout);
    if (written != count) {
        snapshot_offset = snapshot_size;
        printf("\n[usb-trace] UART replay FAILED (short write)\n");
        return;
    }
    snapshot_offset += count;
    if (snapshot_offset == snapshot_size) printf("\n[usb-trace] END\n");
}
