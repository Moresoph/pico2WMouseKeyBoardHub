#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include "../src/hub_usb_mouse.c"

static uint32_t now;
static bool mounted[CFG_TUH_HID], submit = true;
static unsigned submissions;
uint32_t btstack_run_loop_get_time_ms(void) { return now; }
void tuh_hid_set_default_protocol(uint8_t protocol) { assert(protocol == HID_PROTOCOL_REPORT); }
bool tuh_hid_mounted(uint8_t address, uint8_t index) { (void)address; return mounted[index]; }
bool tuh_hid_receive_report(uint8_t address, uint8_t index) {
    assert(address && index < CFG_TUH_HID);
    ++submissions;
    return submit;
}
uint8_t tuh_hid_interface_protocol(uint8_t address, uint8_t index) {
    (void)address; (void)index; return 2;
}
void hci_dump_log(int level, const char *format, ...) { (void)level; (void)format; }
void btstack_assert_failed(const char *file, const uint16_t line) {
    fprintf(stderr, "BTstack assertion %s:%u\n", file, line);
    assert(false);
}

typedef struct { bool unused; } hidh_interface_t;
typedef struct { uint8_t epin[64], epout[64]; } hidh_epbuf_t;
static hidh_interface_t hid;
static hidh_epbuf_t buffers;
static uint8_t get_idx_by_epaddr(uint8_t address, uint8_t ep) { (void)address; (void)ep; return 0; }
static hidh_interface_t *get_hid_itf(uint8_t address, uint8_t index) {
    (void)address; return index < CFG_TUH_HID && mounted[index] ? &hid : NULL;
}
static hidh_epbuf_t *get_hid_epbuf(uint8_t index) { (void)index; return &buffers; }
static void (*tuh_hid_report_sent_cb)(uint8_t, uint8_t, const uint8_t *, uint16_t);
#define tu_edpt_dir(address) ((address) >> 7)
#define TUSB_DIR_IN 1
#define TU_VERIFY(value) do { if (!(value)) return false; } while (0)
#define TU_LOG_DRV(...) ((void)0)
#define TU_LOG3_MEM(...) ((void)0)
#include "usb_hid_xfer.inc"

static const uint8_t mouse8[] = {
    0x05,1,0x09,2,0xa1,1,0x09,1,0xa1,0,
    0x05,9,0x19,1,0x29,3,0x15,0,0x25,1,0x95,3,0x75,1,0x81,2,
    0x95,1,0x75,5,0x81,1,
    0x05,1,0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7f,0x75,8,0x95,3,0x81,6,
    0xc0,0xc0
};
static const uint8_t mouse16[] = {
    0x05,1,0x09,2,0xa1,1,0x85,2,0x09,0x30,0x09,0x31,
    0x16,0,0x80,0x26,0xff,0x7f,0x75,16,0x95,2,0x81,6,0xc0
};
static const uint8_t mouse12[] = {
    0x05,1,0x09,2,0xa1,1,0x85,3,0x09,0x30,0x09,0x31,
    0x16,0,0xf8,0x26,0xff,7,0x75,12,0x95,2,0x81,6,0xc0
};
int main(void) {
    hub_usb_mouse_layout_t layout;
    hub_usb_mouse_motion_t motion;
    const uint8_t report[] = {5,7,0xfb,1};
    assert(hub_usb_mouse_layout(&layout, mouse8, sizeof(mouse8)));
    assert(hub_usb_mouse_decode(&layout, report, sizeof(report), &motion));
    assert(motion.dx == 7 && motion.dy == -5 && motion.wheel == 1 && motion.buttons == 5);
    assert(!hub_usb_mouse_decode(&layout, report, sizeof(report) - 1, &motion));
    assert(!hub_usb_mouse_decode(&layout, NULL, 0, &motion));
    assert(hub_usb_mouse_layout(&layout, mouse16, sizeof(mouse16)));
    uint8_t wide[] = {2,0,0x80,0xff,0x7f};
    assert(hub_usb_mouse_decode(&layout, wide, sizeof(wide), &motion));
    assert(motion.dx == -32768 && motion.dy == 32767);
    wide[0] = 4;
    assert(!hub_usb_mouse_decode(&layout, wide, sizeof(wide), &motion));
    assert(hub_usb_mouse_layout(&layout, mouse12, sizeof(mouse12)));
    const uint8_t packed[] = {3,0xff,0xff,7};
    assert(hub_usb_mouse_decode(&layout, packed, sizeof(packed), &motion));
    assert(motion.dx == -1 && motion.dy == 127);
    uint8_t absolute[sizeof(mouse16)];
    memcpy(absolute, mouse16, sizeof(absolute));
    absolute[sizeof(absolute) - 2] = 2;
    assert(!hub_usb_mouse_layout(&layout, absolute, sizeof(absolute)));
    assert(!hub_usb_mouse_decode(&layout, wide, sizeof(wide), &motion));
    assert(!hub_usb_mouse_layout(&layout, mouse8, sizeof(mouse8) - 1));
    uint8_t invalid[] = {0x75,33,0x95,1,0x81,6};
    assert(!hub_usb_mouse_layout(&layout, invalid, sizeof(invalid)));
    assert(!hub_usb_mouse_layout(&layout, NULL, 0));

    hub_usb_mouse_init();
    mounted[0] = true;
    tuh_hid_mount_cb(1, 0, mouse8, sizeof(mouse8));
    assert(mice[0].usable && mice[0].pending && submissions == 1);
    tuh_hid_report_received_cb(1, 0, report, sizeof(report));
    assert(mice[0].moves == 1 && mice[0].total_dx == 7 && mice[0].total_dy == -5 && submissions == 2);
    const uint8_t neutral[] = {0,0,0,0};
    tuh_hid_report_received_cb(1, 0, neutral, sizeof(neutral));
    assert(mice[0].decoded == 2 && mice[0].moves == 1);
    tuh_hid_report_received_cb(2, 0, report, sizeof(report));
    assert(mice[0].reports == 2); /* A callback for a different device cannot add motion. */
    tuh_hid_umount_cb(1, 0);
    tuh_hid_report_received_cb(1, 0, report, sizeof(report));
    assert(!mice[0].mounted && !mice[0].moves);

    submit = false;
    now = 0;
    tuh_hid_mount_cb(2, 0, mouse8, sizeof(mouse8));
    assert(!mice[0].pending && mice[0].errors == 1);
    unsigned before = submissions;
    now = 99;
    hub_usb_mouse_poll();
    assert(submissions == before);
    submit = true;
    now = 100;
    hub_usb_mouse_poll();
    assert(mice[0].pending && submissions == before + 1);

    memcpy(buffers.epin, report, sizeof(report));
    assert(hidh_xfer_cb(2, 0x81, XFER_RESULT_FAILED, sizeof(report)));
    assert(!mice[0].moves && mice[0].errors == 2 && !mice[0].pending);
    now = 200;
    hub_usb_mouse_poll();
    assert(hidh_xfer_cb(2, 0x81, XFER_RESULT_SUCCESS, sizeof(report)));
    assert(mice[0].moves == 1 && mice[0].last.dx == 7 && mice[0].pending);
    now = 1000;
    hub_usb_mouse_poll();
    hub_usb_mouse_status();
    assert(mice[0].logged_moves == 1);
    tuh_hid_umount_cb(2, 0);
    tuh_hid_mount_cb(3, 0, NULL, 0);
    assert(!mice[0].usable && !mice[0].pending && !mice[0].moves);
    puts("USB receiver: signed 8/12/16-bit reports, IDs, bounds, re-arm, unplug and failed-transfer isolation passed");
    return 0;
}
