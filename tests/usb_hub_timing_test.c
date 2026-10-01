#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "hub_usb_host.h"

#define CFG_TUH_MEM_ALIGN
#define CFG_TUH_MEM_SECTION
#define TU_ATTR_ALWAYS_INLINE
#define TU_ASSERT(condition, ...) do { assert(condition); } while (0)
#define TU_VERIFY(condition, ...) do { if (!(condition)) return; } while (0)
#define TU_LOG1(...) ((void)0)
#define TU_LOG_DRV(...) ((void)0)
#define tu_memclr(buffer, size) memset(buffer, 0, size)
#define tu_le16toh(value) (value)
enum { HUB_FEATURE_PORT_POWER = 8, HCD_EVENT_DEVICE_ATTACH = 1, HCD_EVENT_DEVICE_REMOVE = 2 };
typedef struct {
    union { uint16_t value; struct { uint16_t connection : 1; }; } status;
    struct { uint16_t value; } change;
} hub_port_status_response_t;
typedef hub_port_status_response_t hub_status_response_t;
typedef struct {
    uint8_t bLength, bDescriptorType, bNbrPorts, characteristics[2], bPwrOn2PwrGood;
    uint8_t current, removable, power_mask;
} descriptor_hub_desc_t;
typedef struct { uint16_t wIndex; } request_t;
typedef struct {
    uint8_t daddr;
    xfer_result_t result;
    uint32_t actual_len;
    const request_t* setup;
} tuh_xfer_t;
typedef void (*tuh_xfer_cb_t)(tuh_xfer_t*);
typedef struct {
    uint8_t rhport, event_id;
    struct { uint8_t hub_addr, hub_port; } connection;
} hcd_event_t;

static uint32_t now_ms;
static unsigned powers, notifications, configured, blocked, cancels, attaches, detaches;
static uint8_t power_port;
static bool submit_power = true, submit_notification = true;
static uint64_t time_us_64(void) { return (uint64_t)now_ms * 1000u; }
static bool hub_port_set_feature(uint8_t address, uint8_t port, uint8_t feature,
                                 tuh_xfer_cb_t callback, uintptr_t data) {
    assert(address == 5 && feature == HUB_FEATURE_PORT_POWER && callback && !data);
    ++powers; power_port = port;
    return submit_power;
}
static bool usbh_edpt_xfer(uint8_t address, uint8_t endpoint, void* buffer, uint16_t size) {
    assert(address == 5 && endpoint == 0x81 && buffer && size == 1);
    ++notifications;
    return submit_notification;
}
static void usbh_driver_set_config_complete(uint8_t address, uint8_t interface) {
    assert(address == 5 && !interface); ++configured;
}
void hub_usb_enum_block(uint8_t address, uint8_t port, const char* error) {
    assert(address == 5 && !port && error); ++blocked;
}
void hub_usb_enum_cancel_power_wait(void) { ++cancels; }
static uint8_t usbh_get_rhport(uint8_t address) { assert(address == 5); return 0; }
static void hcd_event_handler(const hcd_event_t* event, bool in_isr) {
    assert(!in_isr && !event->rhport && event->connection.hub_addr == 5 && event->connection.hub_port == 1);
    if (event->event_id == HCD_EVENT_DEVICE_ATTACH) ++attaches;
    else { assert(event->event_id == HCD_EVENT_DEVICE_REMOVE); ++detaches; }
}
#include "usb_hub_timing.inc"

static void power_begin(uint8_t units) {
    assert(hub_init() && sizeof(descriptor_hub_desc_t) == 9 &&
           offsetof(descriptor_hub_desc_t, bPwrOn2PwrGood) == 5);
    get_itf(5)->ep_in = 0x81;
    const uint8_t descriptor[] = {9, 0x29, 4, 0x20, 0, units, 0x64, 0, 0xff};
    memcpy(_hub_buffer, descriptor, sizeof(descriptor));
    tuh_xfer_t xfer = {.daddr = 5, .result = XFER_RESULT_SUCCESS, .actual_len = 9};
    config_set_port_power(&xfer);
    assert(get_itf(5)->power_good_ms == (uint16_t)units * 2u && power_port == 1);
    for (uint16_t port = 1; port <= 4; ++port) {
        request_t request = {.wIndex = port}; xfer.setup = &request;
        config_port_power_complete(&xfer);
        if (port < 4) assert(power_port == port + 1 && !hub_usb_hub_power_pending());
    }
    assert(hub_usb_hub_power_pending());
}

int main(void) {
    const uint8_t units[] = {0x30, 0xff, 0};
    for (unsigned i = 0; i < sizeof(units); ++i) {
        now_ms = UINT32_MAX - 20u;
        unsigned before = configured, notify_before = notifications;
        power_begin(units[i]);
        uint16_t delay = (uint16_t)units[i] * 2u;
        if (delay) {
            now_ms += delay - 1u;
            hub_usb_hub_poll();
            assert(configured == before && notifications == notify_before);
            ++now_ms;
        }
        hub_usb_hub_poll();
        assert(configured == before + 1 && notifications == notify_before + 1 && !hub_usb_hub_power_pending());
        hub_usb_hub_poll();
        assert(configured == before + 1);
    }
    assert(powers == 12 && configured == 3);

    power_begin(0x30);
    hub_close(5);
    now_ms += 1000;
    hub_usb_hub_poll();
    assert(configured == 3 && cancels == 1 && !hub_usb_hub_power_pending());

    power_begin(0x30);
    submit_notification = false;
    now_ms += 96;
    hub_usb_hub_poll();
    assert(blocked == 1 && configured == 3 && !hub_usb_hub_power_pending());
    submit_notification = true;

    tuh_xfer_t bad = {.daddr = 5, .result = XFER_RESULT_SUCCESS, .actual_len = 5};
    config_set_port_power(&bad);
    assert(blocked == 2);
    bad.result = XFER_RESULT_FAILED;
    config_set_port_power(&bad);
    config_port_power_complete(&bad);
    assert(blocked == 4);
    bad.result = XFER_RESULT_SUCCESS; bad.actual_len = 9;
    submit_power = false;
    config_set_port_power(&bad);
    assert(blocked == 5);
    submit_power = true;
    _hub_buffer[2] = 0;
    config_set_port_power(&bad);
    _hub_buffer[2] = 8;
    config_set_port_power(&bad);
    assert(blocked == 7);
    _hub_buffer[2] = 4;

    request_t request = {.wIndex = 1};
    tuh_xfer_t connected = {.daddr = 5, .result = XFER_RESULT_SUCCESS, .setup = &request};
    get_itf(5)->port_status.status.connection = 1;
    connection_clear_conn_change_complete(&connected);
    assert(attaches == 1 && !detaches);
    get_itf(5)->port_status.status.connection = 0;
    connection_clear_conn_change_complete(&connected);
    assert(attaches == 1 && detaches == 1);
    assert(hub_deinit());
    puts("USB Hub: actual callbacks enforce power-good 0/96/510ms, wrap, removal, errors and attach without early reset");
    return 0;
}
