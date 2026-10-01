#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "btstack_run_loop.h"
#include "hub_usb_mouse.h"
#include "hub_usb_mouse_decode.h"

typedef struct {
    bool mounted, usable, pending;
    uint8_t address;
    uint32_t retry_at, last_log, reports, decoded, moves, errors, ignored, logged_moves;
    int64_t total_dx, total_dy;
    hub_usb_mouse_motion_t last;
    hub_usb_mouse_layout_t layout;
} usb_mouse_t;

static usb_mouse_t mice[CFG_TUH_HID];

void hub_usb_mouse_init(void) {
    memset(mice, 0, sizeof(mice));
    tuh_hid_set_default_protocol(HID_PROTOCOL_REPORT);
}

static void receive_next(uint8_t index) {
    usb_mouse_t *mouse = &mice[index];
    if (!mouse->mounted || !mouse->usable || mouse->pending) return;
    if (!tuh_hid_mounted(mouse->address, index)) return;
    if (tuh_hid_receive_report(mouse->address, index)) {
        mouse->pending = true;
    } else {
        ++mouse->errors;
        mouse->retry_at = btstack_run_loop_get_time_ms() + 100u;
        if ((mouse->errors & (mouse->errors - 1u)) == 0)
            printf("[usb-mouse] receive submission FAILED addr=%u hid=%u errors=%" PRIu32 "\n",
                   mouse->address, index, mouse->errors);
    }
}

void tuh_hid_mount_cb(uint8_t address, uint8_t index, const uint8_t *descriptor, uint16_t length) {
    if (!address || index >= CFG_TUH_HID) {
        printf("[usb-mouse] invalid HID mount addr=%u hid=%u\n", address, index);
        return;
    }
    usb_mouse_t *mouse = &mice[index];
    memset(mouse, 0, sizeof(*mouse));
    mouse->mounted = true;
    mouse->address = address;
    mouse->usable = hub_usb_mouse_layout(&mouse->layout, descriptor, length);
    printf("[usb-mouse] mounted addr=%u hid=%u interface-protocol=%u descriptor=%u relative-mouse=%u\n",
           address, index, tuh_hid_interface_protocol(address, index), length, mouse->usable);
    if (!mouse->usable) {
        printf("[usb-mouse] ignored: no supported relative mouse layout (keyboard/vendor/unsupported descriptor)\n");
        return;
    }
    receive_next(index);
}

void tuh_hid_umount_cb(uint8_t address, uint8_t index) {
    if (index >= CFG_TUH_HID || mice[index].address != address) return;
    printf("[usb-mouse] removed addr=%u hid=%u reports=%" PRIu32 " moves=%" PRIu32 "\n",
           address, index, mice[index].reports, mice[index].moves);
    memset(&mice[index], 0, sizeof(mice[index]));
}

void tuh_hid_report_received_cb(uint8_t address, uint8_t index, const uint8_t *report, uint16_t length) {
    if (index >= CFG_TUH_HID) return;
    usb_mouse_t *mouse = &mice[index];
    if (!mouse->mounted || !mouse->usable || mouse->address != address ||
        !tuh_hid_mounted(address, index)) return;
    mouse->pending = false;
    if (!length) {
        ++mouse->errors;
        mouse->retry_at = btstack_run_loop_get_time_ms() + 100u;
        printf("[usb-mouse] input transfer FAILED addr=%u hid=%u\n", address, index);
        return;
    }
    ++mouse->reports;
    hub_usb_mouse_motion_t motion;
    if (hub_usb_mouse_decode(&mouse->layout, report, length, &motion)) {
        ++mouse->decoded;
        mouse->last = motion;
        if (motion.dx || motion.dy) {
            ++mouse->moves;
            mouse->total_dx += motion.dx;
            mouse->total_dy += motion.dy;
            if (mouse->moves == 1) {
                printf("[usb-mouse] FIRST MOTION addr=%u hid=%u dx=%" PRId32 " dy=%" PRId32
                       " buttons=%02x; capture-only, NOT forwarded\n",
                       address, index, motion.dx, motion.dy, motion.buttons);
            }
        }
    } else {
        ++mouse->ignored;
        if (mouse->ignored == 1)
            printf("[usb-mouse] non-mouse/invalid report addr=%u hid=%u bytes=%u\n", address, index, length);
    }
    receive_next(index);
}

void hub_usb_mouse_poll(void) {
    uint32_t now = btstack_run_loop_get_time_ms();
    for (uint8_t i = 0; i < CFG_TUH_HID; ++i) {
        usb_mouse_t *mouse = &mice[i];
        if (!mouse->mounted || !mouse->usable) continue;
        if (!mouse->pending && (int32_t)(now - mouse->retry_at) >= 0) receive_next(i);
        if (mouse->moves != mouse->logged_moves && (uint32_t)(now - mouse->last_log) >= 500u) {
            printf("[usb-mouse] hid=%u moves=%" PRIu32 " total-dx=%" PRId64 " total-dy=%" PRId64 "\n",
                   i, mouse->moves, mouse->total_dx, mouse->total_dy);
            mouse->logged_moves = mouse->moves;
            mouse->last_log = now;
        }
    }
}

void hub_usb_mouse_status(void) {
    unsigned count = 0;
    printf("[usb-mouse] HID-receive=1 PC-forwarding=0\n");
    for (uint8_t i = 0; i < CFG_TUH_HID; ++i) {
        const usb_mouse_t *mouse = &mice[i];
        if (!mouse->mounted) continue;
        ++count;
        printf("[usb-mouse] addr=%u hid=%u usable=%u pending=%u reports=%" PRIu32
               " decoded=%" PRIu32 " moves=%" PRIu32 " errors=%" PRIu32 " ignored=%" PRIu32 "\n",
               mouse->address, i, mouse->usable, mouse->pending, mouse->reports,
               mouse->decoded, mouse->moves, mouse->errors, mouse->ignored);
        printf("[usb-mouse] last dx=%" PRId32 " dy=%" PRId32 " wheel=%" PRId32
               " buttons=%02x total-dx=%" PRId64 " total-dy=%" PRId64 "\n",
               mouse->last.dx, mouse->last.dy, mouse->last.wheel, mouse->last.buttons,
               mouse->total_dx, mouse->total_dy);
    }
    if (!count) printf("[usb-mouse] no HID interfaces mounted\n");
}
