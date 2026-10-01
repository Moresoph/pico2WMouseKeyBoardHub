#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static char output[20000];
static size_t output_used, largest_write;
static uint32_t irq_state;
static bool fail_format, short_write;
static int capture_printf(const char *format, ...);
static size_t capture_write(const void *data, size_t size, size_t count, FILE *stream);
static int format_text(char *data, size_t size, const char *format, va_list args);
#define printf capture_printf
#define fwrite capture_write
#define vsnprintf format_text
#include "../src/hub_usb_trace.c"
#undef printf
#undef fwrite
#undef vsnprintf

uint32_t save_and_disable_interrupts(void) {
    uint32_t previous = irq_state;
    irq_state = 1;
    return previous;
}
void restore_interrupts(uint32_t previous) {
    assert(irq_state == 1);
    irq_state = previous;
}
static int format_text(char *data, size_t size, const char *format, va_list args) {
    if (fail_format) return -1;
#if defined(__TINYC__) && defined(_WIN32)
    /* TCC's legacy MSVCRT truncation return differs from the firmware's C99 API. */
    va_list copy;
    va_copy(copy, args);
    int length = _vscprintf(format, copy);
    va_end(copy);
    vsnprintf(data, size, format, args);
    if (size) data[size - 1] = 0;
    return length;
#else
    return vsnprintf(data, size, format, args);
#endif
}
static int capture_printf(const char *format, ...) {
    assert(!irq_state);
    va_list args;
    va_start(args, format);
    int length = vsnprintf(output + output_used, sizeof(output) - output_used, format, args);
    va_end(args);
    assert(length >= 0 && (size_t)length < sizeof(output) - output_used);
    output_used += (size_t)length;
    return length;
}
static size_t capture_write(const void *data, size_t size, size_t count, FILE *stream) {
    assert(!irq_state && size == 1 && stream == stdout && count <= USB_TRACE_CHUNK);
    if (count > largest_write) largest_write = count;
    if (short_write) --count;
    assert(output_used + count < sizeof(output));
    memcpy(output + output_used, data, count);
    output_used += count;
    output[output_used] = 0;
    return count;
}
static void reset_test(void) {
    next = used = snapshot_size = snapshot_offset = 0;
    overwritten = truncated = format_errors = 0;
    output_used = largest_write = 0;
    output[0] = 0;
}
int main(void) {
    hub_usb_trace_dump();
    assert(strstr(output, "END (empty)"));
    hub_usb_trace_poll();
    reset_test();
    irq_state = 1;
    assert(hub_usb_trace_printf("test %u\n", 7) == 7);
    assert(irq_state == 1 && !output_used); /* No UART, including from an ISR. */
    irq_state = 0;
    hub_usb_trace_dump();
    assert(snapshot_size == 7 && memcmp(snapshot, "test 7\n", 7) == 0);
    hub_usb_trace_dump();
    assert(strstr(output, "already in progress"));
    hub_usb_trace_printf("new data\n");
    assert(snapshot_size == 7 && memcmp(snapshot, "test 7\n", 7) == 0);
    hub_usb_trace_poll();
    assert(strstr(output, "test 7\n\n[usb-trace] END"));
    assert(snapshot_offset == snapshot_size);

    reset_test();
    for (unsigned i = 0; i < USB_TRACE_BYTES + 37; ++i)
        hub_usb_trace_printf("%c", 'A' + i % 26);
    assert(used == USB_TRACE_BYTES && overwritten == 37);
    hub_usb_trace_dump();
    for (unsigned i = 0; i < USB_TRACE_BYTES; ++i)
        assert(snapshot[i] == 'A' + (i + 37) % 26);
    while (snapshot_offset < snapshot_size) {
        hub_usb_trace_printf("live\n");
        hub_usb_trace_poll();
    }
    assert(largest_write == USB_TRACE_CHUNK);
    assert(strstr(output, "[usb-trace] END"));

    reset_test();
    char long_text[401];
    memset(long_text, 'x', sizeof(long_text) - 1);
    long_text[400] = 0;
    assert(hub_usb_trace_printf("%s", long_text) == 400);
    assert(truncated == 1 && used == 191);
    fail_format = true;
    assert(hub_usb_trace_printf("invalid") == -1);
    assert(format_errors == 1 && used == 191 && !irq_state);
    fail_format = false;
    hub_usb_trace_dump();
    short_write = true;
    hub_usb_trace_poll();
    short_write = false;
    assert(snapshot_offset == snapshot_size && strstr(output, "replay FAILED"));

    used = USB_TRACE_BYTES;
    overwritten = UINT32_MAX;
    hub_usb_trace_printf("x");
    assert(overwritten == UINT32_MAX);
    puts("USB trace: bounded ISR capture, chronological wrap, isolated paced replay and errors passed");
    return 0;
}
