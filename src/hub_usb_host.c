#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "btstack_run_loop.h"
#include "tusb.h"
#include "host/usbh_pvt.h"
#include "host/hcd.h"
#include "hub_usb_descriptor.h"
#include "hub_usb_host.h"
#include "hub_usb_trace.h"
#include "hub_usb_mouse.h"
#include "hub_usb_wire.h"

#define USB_ADDRESSES (CFG_TUH_DEVICE_MAX + CFG_TUH_HUB + 1)

typedef struct {
    bool seen, announced, hub, bluetooth_hci;
    uint16_t vid, pid;
    hub_usb_interface_t hci;
} usb_peer_t;

static usb_peer_t peers[USB_ADDRESSES];
static btstack_timer_source_t usb_timer;
static bool started;
static uint32_t mounts, removals, malformed, polls;

static bool observer_init(void) {
    memset(peers, 0, sizeof(peers));
    mounts = removals = malformed = polls = 0;
    return true;
}

static bool observer_deinit(void) {
    return true;
}

static bool observer_open(uint8_t rhport, uint8_t address,
                          const tusb_desc_interface_t *descriptor, uint16_t length) {
    (void)rhport;
    if (!address || address >= USB_ADDRESSES) {
        printf("[usb] unsupported device address=%u\n", address);
        return false;
    }
    hub_usb_interface_t info;
    if (!hub_usb_inspect_interface((const uint8_t *)descriptor, length, &info)) {
        ++malformed;
        printf("[usb] malformed interface at addr=%u length=%u\n", address, length);
        return false;
    }
    usb_peer_t *peer = &peers[address];
    if (!tuh_vid_pid_get(address, &peer->vid, &peer->pid)) {
        printf("[usb] VID/PID unavailable at addr=%u\n", address);
        return false;
    }
    peer->seen = true;
    peer->hub |= info.class_code == 9;
    printf("[usb] addr=%u VID:PID=%04x:%04x itf=%u alt=%u class=%02x/%02x/%02x\n",
           address, peer->vid, peer->pid, info.number, info.alternate,
           info.class_code, info.subclass, info.protocol);
    if (info.bluetooth_hci) {
        peer->bluetooth_hci = true;
        peer->hci = info;
        printf("[usb] Bluetooth HCI candidate: event-IN=%02x/%u ACL-IN=%02x/%u ACL-OUT=%02x/%u\n",
               info.event_in, info.event_size, info.acl_in, info.acl_in_size,
               info.acl_out, info.acl_out_size);
    }
    /* Observe only: leave the hub to its driver; do not start a second BTstack. */
    return false;
}

static void observer_close(uint8_t address) {
    if (!address || address >= USB_ADDRESSES || !peers[address].seen) return;
    printf("[usb] removed addr=%u VID:PID=%04x:%04x\n",
           address, peers[address].vid, peers[address].pid);
    memset(&peers[address], 0, sizeof(peers[address]));
    ++removals;
}

static const usbh_class_driver_t observer = {
    .name = "Hub USB observer",
    .init = observer_init,
    .deinit = observer_deinit,
    .open = observer_open,
    .close = observer_close
};

const usbh_class_driver_t *usbh_app_driver_get_cb(uint8_t *count) {
    *count = 1;
    return &observer;
}

static void usb_poll(btstack_timer_source_t *timer) {
    ++polls;
    tuh_task_ext(0, false);
    hub_usb_hub_poll();
    hub_usb_enum_poll();
    hub_usb_mouse_poll();
    hub_usb_trace_poll();
    hub_usb_wire_poll();
    for (uint8_t address = 1; address < USB_ADDRESSES; ++address) {
        usb_peer_t *peer = &peers[address];
        if (!peer->seen || peer->announced || !tuh_mounted(address)) continue;
        peer->announced = true;
        ++mounts;
        printf("[usb] configured addr=%u %s speed=%u (0=full,1=low,2=high)\n",
               address, peer->hub ? "hub" : peer->bluetooth_hci ? "Bluetooth HCI candidate" : "unclaimed device",
               (unsigned)tuh_speed_get(address));
    }
    btstack_run_loop_set_timer(timer, 1);
    btstack_run_loop_add_timer(timer);
}

bool hub_usb_host_init(void) {
    if (started) return true;
    hub_usb_mouse_init();
    const tusb_rhport_init_t config = {.role = TUSB_ROLE_HOST, .speed = TUSB_SPEED_FULL};
    if (!tusb_init(0, &config)) {
        printf("[usb] native Host initialization FAILED\n");
        return false;
    }
    started = true;
    btstack_run_loop_set_timer_handler(&usb_timer, usb_poll);
    btstack_run_loop_set_timer(&usb_timer, 1);
    btstack_run_loop_add_timer(&usb_timer);
    printf("[usb] Host probe ready; UART0 GP0=TX GP1=RX 115200 8N1\n"
           "[usb] USB HID mouse capture enabled; external Bluetooth NOT active; onboard Bluetooth unchanged\n"
           "[usb] requires correctly supplied Host VBUS; never connect this running Host port to a PC\n");
    return true;
}

void hub_usb_host_status(void) {
    printf("[usb] host=%u enumeration-only=0 USB-HID-capture=1 external-Bluetooth=0 mounts=%" PRIu32
           " removals=%" PRIu32 " malformed=%" PRIu32 "\n", started, mounts, removals, malformed);
    if (started) {
        printf("[usb] root-data-connected=%u frame=%" PRIu32 " task-polls=%" PRIu32
               " queued-events=%u (USB VBUS override is NOT a voltage measurement)\n",
               hcd_port_connect_status(0), hcd_frame_number(0), polls, tuh_task_event_ready());
        hub_usb_hcd_state_t hcd;
        hub_usb_hcd_get_state(&hcd);
        printf("[usb-hcd] EP0 active=%u address=%u ep=%02x mps=%u remaining=%" PRIu32
               " transferred=%" PRIu32 " RX-timeouts=%" PRIu32 "\n",
               hcd.active, hcd.address, hcd.endpoint, hcd.packet_size,
               hcd.remaining, hcd.transferred, hcd.rx_timeouts);
        printf("[usb-hcd] sie-ctrl=%08" PRIx32 " sie-status=%08" PRIx32
               " ints=%08" PRIx32 " inte=%08" PRIx32 " addr=%08" PRIx32
               " buffer=%08" PRIx32 "\n",
               hcd.sie_ctrl, hcd.sie_status, hcd.ints, hcd.inte,
               hcd.address_control, hcd.buffer_control);
        if (hcd.hub_timing_supported) {
            printf("[usb-hcd] linestate-tuning=%08" PRIx32 " Hub-turnaround-fix=%u\n",
                  hcd.linestate_tuning, (unsigned)hcd.hub_timing_enabled);
        } else {
            printf("[usb-hcd] Hub-turnaround-fix=unsupported on this MCU\n");
        }
        hub_usb_trace_status();
        hub_usb_enum_status();
        hub_usb_wire_status();
    }
    unsigned present = 0;
    for (uint8_t address = 1; address < USB_ADDRESSES; ++address) {
        const usb_peer_t *peer = &peers[address];
        if (!peer->seen) continue;
        ++present;
        printf("[usb] addr=%u VID:PID=%04x:%04x configured=%u hub=%u HCI-candidate=%u\n",
               address, peer->vid, peer->pid, tuh_mounted(address), peer->hub, peer->bluetooth_hci);
        if (peer->bluetooth_hci) {
            printf("[usb] itf=%u event-IN=%02x/%u ACL-IN=%02x/%u ACL-OUT=%02x/%u\n",
                   peer->hci.number, peer->hci.event_in, peer->hci.event_size,
                   peer->hci.acl_in, peer->hci.acl_in_size, peer->hci.acl_out, peer->hci.acl_out_size);
        }
    }
    if (!present)
        printf("[usb] no interfaces observed; check upstream cable, external 5V/VBUS and TinyUSB enumeration errors\n");
    hub_usb_mouse_status();
}
