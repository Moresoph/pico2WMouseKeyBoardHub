#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "hub_usb_host.h"

enum {
    HUB_FEATURE_PORT_CONNECTION = 0, HUB_FEATURE_PORT_ENABLE = 1,
    HUB_FEATURE_PORT_OVER_CURRENT = 3, HUB_FEATURE_PORT_RESET = 4,
    HUB_FEATURE_PORT_POWER = 8, HUB_FEATURE_PORT_LOW_SPEED = 9,
    HUB_FEATURE_PORT_CONNECTION_CHANGE = 16, HUB_FEATURE_PORT_RESET_CHANGE = 20,
    HCD_EVENT_DEVICE_ATTACH = 1, CONTROL_STAGE_IDLE = 0
};
typedef struct {
    uint8_t event_id, rhport;
    struct { uint8_t hub_addr, hub_port; } connection;
} hcd_event_t;
typedef struct { struct { uint16_t value; } status, change; } hub_port_status_response_t;
typedef struct {
    uint8_t daddr;
    xfer_result_t result;
    uint32_t actual_len;
    uintptr_t user_data;
} tuh_xfer_t;
typedef void (*tuh_xfer_cb_t)(tuh_xfer_t*);
static struct { uint8_t rhport, hub_addr, hub_port, speed; bool enumerating; } _dev0;
static struct { uint8_t stage; } _ctrl_xfer;
static struct { uint8_t ctrl[1024]; } _usbh_epbuf;
static uint32_t clock_ms;
static unsigned disables, reads, resumes, closes, removals, retries, successful_prefix_calls, logs;
static unsigned queued_attaches, allowed_attaches;
static unsigned reset_requests, clear_requests, descriptors, sleep_calls, root_resets, root_ends;
static uint32_t reset_request_ms, descriptor_ms;
static bool submit_reset = true, submit_clear = true, power_pending;
static void queue_event(const hcd_event_t* event, bool in_isr) {
    assert(event->event_id == HCD_EVENT_DEVICE_ATTACH && !in_isr); ++queued_attaches;
}
static bool submit_disable = true, submit_read = true, submit_retry = true;
static tuh_xfer_cb_t pending;
static uintptr_t pending_generation;
static hub_port_status_response_t* response;
static uint8_t pending_hub;
#define tu_le16toh(value) (value)
static void log_message(const char* format, ...) { (void)format; ++logs; }
#define TU_LOG1 log_message
#define TU_LOG_USBH log_message
#define TU_ASSERT(condition, ...) do { assert(condition); } while (0)
static const char* const tu_str_speed[] = {"full", "low", "high"};
static uint64_t time_us_64(void) { return (uint64_t)clock_ms * 1000u; }
static void tusb_time_delay_ms_api(uint32_t ms) {
    assert(ms == 100 || ms == 50 || ms == 450); clock_ms += ms; ++sleep_calls;
}
bool hub_usb_hub_power_pending(void) { return power_pending; }
static void hcd_port_reset(uint8_t rhport) { assert(!rhport); ++root_resets; }
static void hcd_port_reset_end(uint8_t rhport) { assert(!rhport); ++root_ends; }
static bool hcd_port_connect_status(uint8_t rhport) { assert(!rhport); return true; }
static tusb_speed_t hcd_port_speed_get(uint8_t rhport) { assert(!rhport); return TUSB_SPEED_FULL; }
static bool usbh_edpt_control_open(uint8_t address, uint16_t packet_size) {
    assert(!address && packet_size == 8); return true;
}
static bool tuh_descriptor_get_device(uint8_t address, void* buffer, uint16_t length,
                                      tuh_xfer_cb_t callback, uintptr_t state);
static bool tuh_control_xfer(tuh_xfer_t* xfer) { (void)xfer; ++retries; return submit_retry; }
static bool hub_port_clear_feature(uint8_t hub, uint8_t port, uint8_t feature,
                                   tuh_xfer_cb_t callback, uintptr_t generation) {
    assert(_dev0.enumerating && hub == _dev0.hub_addr && port == _dev0.hub_port);
    assert(port == 1 || port == 4);
    pending = callback; pending_generation = generation; pending_hub = hub;
    if (feature == HUB_FEATURE_PORT_ENABLE) { ++disables; return submit_disable; }
    assert(feature == HUB_FEATURE_PORT_CONNECTION_CHANGE || feature == HUB_FEATURE_PORT_RESET_CHANGE);
    ++clear_requests;
    return submit_clear;
}
static bool hub_port_reset(uint8_t hub, uint8_t port, tuh_xfer_cb_t callback, uintptr_t token) {
    assert(_dev0.enumerating && hub == _dev0.hub_addr && port == _dev0.hub_port);
    ++reset_requests; reset_request_ms = clock_ms;
    pending = callback; pending_generation = token; pending_hub = hub;
    return submit_reset;
}
static bool hub_port_get_status(uint8_t hub, uint8_t port, void* buffer,
                                tuh_xfer_cb_t callback, uintptr_t generation) {
    assert(hub == _dev0.hub_addr && port == _dev0.hub_port);
    ++reads;
    response = buffer;
    pending = callback; pending_generation = generation; pending_hub = hub;
    return submit_read;
}
static void hcd_device_close(uint8_t rhport, uint8_t address) {
    assert(rhport == 0 && address == 0); ++closes;
}
static void process_removing_device(uint8_t rhport, uint8_t hub, uint8_t port) {
    assert(!rhport && hub == _dev0.hub_addr && port == _dev0.hub_port); ++removals;
}
static void enum_full_complete(void) { _dev0.enumerating = false; ++resumes; }
#include "usb_enum_recovery.inc"

static bool tuh_descriptor_get_device(uint8_t address, void* buffer, uint16_t length,
                                      tuh_xfer_cb_t callback, uintptr_t state) {
    assert(!address && buffer == _usbh_epbuf.ctrl && length == 8 &&
           callback == process_enumeration && state == ENUM_SET_ADDR);
    ++descriptors; descriptor_ms = clock_ms;
    return true;
}

static tuh_xfer_t completion(xfer_result_t result, uint32_t length) {
    tuh_xfer_t xfer = {.daddr = pending_hub, .result = result, .actual_len = length,
                       .user_data = pending_generation};
    return xfer;
}
static void begin(uint8_t port) {
    _dev0.rhport = 0; _dev0.hub_addr = 5; _dev0.hub_port = port; _dev0.enumerating = true;
}
static void fail_requests(void) {
    tuh_xfer_t failed = {.result = XFER_RESULT_FAILED};
    for (unsigned i = 0; i < 4; ++i) process_enumeration(&failed);
}
static void verify_disabled(void) {
    tuh_xfer_t reply = completion(XFER_RESULT_SUCCESS, 0);
    pending(&reply);
    assert(hub_enum_cleanup_pending() && _dev0.enumerating && !hub_usb_enum_can_rearm());
    response->status.value = 0x0101;
    reply = completion(XFER_RESULT_SUCCESS, 4);
    pending(&reply);
    assert(!hub_enum_cleanup_pending() && !hub_enum_cleanup_blocked() && !_dev0.enumerating);
    assert(hub_usb_enum_can_rearm());
}

static void prepare_reply(xfer_result_t result, uint32_t length) {
    tuh_xfer_t reply = completion(result, length);
    pending(&reply);
}
static void port_reply(uint16_t status, uint16_t change) {
    response->status.value = status;
    response->change.value = change;
    prepare_reply(XFER_RESULT_SUCCESS, 4);
}
static void prepare_begin(uint8_t port) {
    begin(port);
    hcd_event_t event = {.rhport = 0, .connection = {5, port}};
    assert(enum_new_device(&event) && hub_enum_prepare.active && !hub_usb_enum_can_rearm());
}
static void advance_poll(uint32_t ms) { clock_ms += ms; hub_usb_enum_poll(); }
static void reach_reset(void) {
    uint32_t start = clock_ms;
    unsigned before = reset_requests;
    prepare_begin(1);
    hub_usb_enum_poll();
    tuh_xfer_t old_status = completion(XFER_RESULT_SUCCESS, 4);
    tuh_xfer_cb_t old_callback = pending;
    port_reply(0x0101, 0);
    for (unsigned i = 0; i < 14; ++i) {
        advance_poll(10);
        port_reply(0x0101, 0);
    }
    advance_poll(9);
    assert(reset_requests == before && !hub_enum_prepare.pending);
    advance_poll(1);
    port_reply(0x0101, 0);
    assert(reset_requests == before + 1 && (uint32_t)(reset_request_ms - start) == 150);
    assert(hub_enum_prepare.operation == HUB_PREP_SEND_RESET);
    old_callback(&old_status);
    assert(hub_enum_prepare.pending == submit_reset && hub_enum_prepare.operation == HUB_PREP_SEND_RESET);
}
static void reach_recovery(void) {
    reach_reset();
    prepare_reply(XFER_RESULT_SUCCESS, 0);
    unsigned before = reads;
    advance_poll(19);
    assert(reads == before && !hub_enum_prepare.pending);
    advance_poll(1);
    port_reply(0x0111, 0);
    assert(hub_enum_prepare.phase == HUB_PREP_RESET && !hub_enum_prepare.pending);
    advance_poll(9);
    assert(!hub_enum_prepare.pending);
    advance_poll(1);
    port_reply(0x0103, 0x0010);
    assert(hub_enum_prepare.operation == HUB_PREP_CLEAR_RESET);
    prepare_reply(XFER_RESULT_SUCCESS, 0);
}

static void test_prepare(void) {
    unsigned sleeps = sleep_calls;
    unsigned before = descriptors;
    reach_recovery();
    uint32_t recovered_at = clock_ms;
    advance_poll(9);
    assert(descriptors == before && !hub_enum_prepare.pending);
    advance_poll(1);
    port_reply(0x0103, 0);
    assert(descriptors == before + 1 && (uint32_t)(descriptor_ms - recovered_at) == 10);
    assert(!hub_enum_prepare.active && _dev0.enumerating && _dev0.speed == TUSB_SPEED_FULL);
    enum_full_complete();

    // A disconnect/reconnect observed between polls restarts the full debounce.
    prepare_begin(1);
    hub_usb_enum_poll();
    port_reply(0x0101, 0);
    advance_poll(140);
    port_reply(0x0101, 1);
    assert(hub_enum_prepare.operation == HUB_PREP_CLEAR_CONNECTION);
    prepare_reply(XFER_RESULT_SUCCESS, 0);
    unsigned reset_before = reset_requests, queue_before = queued_attaches, allow_before = allowed_attaches;
    test_attach_dispatch();
    assert(queued_attaches == queue_before && allowed_attaches == allow_before);
    test_attach_dispatch_port(5, 4);
    assert(queued_attaches == queue_before + 1);
    hub_usb_enum_poll();
    port_reply(0x0101, 0);
    advance_poll(149);
    port_reply(0x0101, 0);
    assert(reset_requests == reset_before);
    advance_poll(10);
    port_reply(0x0101, 0);
    assert(reset_requests == reset_before + 1);
    hub_enum_parent_removed(5);
    assert(!_dev0.enumerating && !hub_enum_prepare.active);

    // Stale reset-change is cleared before issuing a new reset.
    prepare_begin(1);
    hub_usb_enum_poll();
    port_reply(0x0101, 0x0010);
    assert(hub_enum_prepare.operation == HUB_PREP_CLEAR_OLD_RESET);
    prepare_reply(XFER_RESULT_SUCCESS, 0);
    advance_poll(150);
    port_reply(0x0101, 0);
    assert(hub_enum_prepare.operation == HUB_PREP_SEND_RESET);
    hub_enum_parent_removed(5);

    prepare_begin(1);
    hub_usb_enum_poll();
    port_reply(0x0111, 0);
    advance_poll(HUB_ENUM_PREPARE_TIMEOUT_MS);
    assert(hub_enum_cleanup.active && !hub_enum_cleanup.blocked && !hub_enum_prepare.active);
    verify_disabled();

    before = descriptors;
    for (unsigned phase = 0; phase < 3; ++phase) {
        if (phase == 0) { prepare_begin(1); hub_usb_enum_poll(); }
        if (phase == 1) { reach_reset(); prepare_reply(XFER_RESULT_SUCCESS, 0); advance_poll(20); }
        if (phase == 2) { reach_recovery(); advance_poll(10); }
        port_reply(0x0100, 1);
        assert(!hub_enum_prepare.active && !_dev0.enumerating && descriptors == before);
    }

    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        unsigned failed_before = disables;
        if (scenario < 4) { prepare_begin(1); hub_usb_enum_poll(); }
        else if (scenario == 4 || scenario == 5) {
            reach_reset(); prepare_reply(XFER_RESULT_SUCCESS, 0);
            advance_poll(scenario == 4 ? 20 : 200);
        } else { reach_recovery(); advance_poll(10); }
        if (scenario == 0) prepare_reply(XFER_RESULT_FAILED, 0);
        if (scenario == 1) prepare_reply(XFER_RESULT_SUCCESS, 2);
        if (scenario == 2) port_reply(0x0001, 0);
        if (scenario == 3) port_reply(0x0109, 0);
        if (scenario == 4) port_reply(0x0101, 0x0010);
        if (scenario == 5) port_reply(0x0111, 0);
        if (scenario == 6) port_reply(0x0103, 1);
        assert(!hub_enum_prepare.active && hub_enum_cleanup.active && disables == failed_before + 1);
        assert(descriptors == before);
        verify_disabled();
    }

    // Failed submissions, including reset and change clearing, use the same cleanup.
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        if (scenario == 0) {
            prepare_begin(1); submit_read = false; hub_usb_enum_poll(); submit_read = true;
        } else if (scenario == 1) {
            submit_reset = false; reach_reset(); submit_reset = true;
        } else {
            prepare_begin(1); hub_usb_enum_poll(); submit_clear = false;
            port_reply(0x0101, 1); submit_clear = true;
        }
        assert(hub_enum_cleanup.active && !hub_enum_prepare.active);
        verify_disabled();
    }

    prepare_begin(1);
    _ctrl_xfer.stage = 1;
    unsigned reads_before = reads;
    advance_poll(10);
    assert(reads == reads_before && !hub_enum_prepare.pending);
    _ctrl_xfer.stage = CONTROL_STAGE_IDLE;
    hub_usb_enum_poll();
    tuh_xfer_t late = completion(XFER_RESULT_SUCCESS, 4);
    tuh_xfer_cb_t late_callback = pending;
    advance_poll(HUB_ENUM_PREPARE_TIMEOUT_MS - 11);
    assert(hub_enum_prepare.active);
    advance_poll(1);
    assert(hub_enum_cleanup.blocked && !hub_enum_prepare.active && !hub_usb_enum_can_rearm());
    late_callback(&late);
    assert(hub_enum_cleanup.blocked);
    hub_enum_parent_removed(5);
    prepare_begin(4);
    hub_usb_enum_poll();
    late_callback(&late);
    assert(hub_enum_prepare.active && hub_enum_prepare.pending);
    port_reply(0x0100, 1);
    assert(!hub_enum_prepare.active);

    // Unsigned elapsed arithmetic is safe across the millisecond wrap.
    clock_ms = UINT32_MAX - 80u;
    reach_recovery();
    advance_poll(10);
    port_reply(0x0303, 0);
    assert(_dev0.speed == TUSB_SPEED_LOW);
    enum_full_complete();
    assert(sleep_calls == sleeps);

    power_pending = true;
    assert(hub_enum_cleanup_pending() && !hub_usb_enum_can_rearm());
    power_pending = false;
    begin(1);
    hub_usb_enum_cancel_power_wait();
    assert(!_dev0.enumerating);

    // Root behavior is deliberately unchanged by the downstream correction.
    hcd_event_t root = {.rhport = 0, .connection = {0, 0}};
    _dev0.enumerating = true;
    before = descriptors;
    assert(enum_new_device(&root));
    assert(root_resets == 1 && root_ends == 1 && sleep_calls == sleeps + 2 && descriptors == before + 1);
    enum_full_complete();
}

int main(void) {
    begin(1);
    tuh_xfer_t success = {.result = XFER_RESULT_SUCCESS};
    process_enumeration(&success);
    assert(successful_prefix_calls == 1 && !disables);
    fail_requests();
    assert(retries == 3 && disables == 1 && !reads && !resumes);
    assert(hub_enum_cleanup_pending());
    test_attach_dispatch();
    assert(queued_attaches == 1 && !allowed_attaches);
    verify_disabled();
    test_attach_dispatch();
    assert(allowed_attaches == 1);
    assert(reads == 1 && resumes == 1 && closes == 1 && removals == 1);
    assert(hub_enum_cleanup.disabled_count == 1);
    begin(4);
    fail_requests();
    assert(retries == 6 && disables == 2 && resumes == 1);
    verify_disabled();
    assert(hub_enum_cleanup.disabled_count == 2 && resumes == 2);

    begin(1);
    fail_requests();
    tuh_xfer_t reply = completion(XFER_RESULT_SUCCESS, 0);
    pending(&reply);
    response->status.value = 0x0103;
    reply = completion(XFER_RESULT_SUCCESS, 4);
    pending(&reply);
    assert(hub_enum_cleanup_blocked() && !hub_usb_enum_can_rearm() && resumes == 2);
    test_attach_dispatch();
    assert(allowed_attaches == 1 && queued_attaches == 1);
    hub_enum_parent_removed(6);
    assert(hub_enum_cleanup_blocked());
    hub_enum_parent_removed(5);
    assert(!hub_enum_cleanup_blocked());
    pending(&reply);
    assert(resumes == 2);

    for (unsigned scenario = 0; scenario < 5; ++scenario) {
        begin(1);
        submit_disable = scenario != 0;
        submit_read = scenario != 2;
        fail_requests();
        if (scenario != 0) {
            reply = completion(scenario == 1 ? XFER_RESULT_FAILED : XFER_RESULT_SUCCESS, 0);
            pending(&reply);
        }
        if (scenario >= 3) {
            reply = completion(scenario == 3 ? XFER_RESULT_FAILED : XFER_RESULT_SUCCESS, 2);
            pending(&reply);
        }
        assert(hub_enum_cleanup_blocked() && !hub_enum_cleanup_pending() && resumes == 2);
        hub_enum_parent_removed(5);
    }
    submit_disable = submit_read = true;
    begin(1);
    fail_requests();
    reply = completion(XFER_RESULT_SUCCESS, 0);
    uint32_t old_generation = hub_enum_cleanup.generation;
    clock_ms += HUB_ENUM_CLEANUP_TIMEOUT_MS - 1;
    hub_usb_enum_poll();
    assert(hub_enum_cleanup_pending());
    ++clock_ms;
    hub_usb_enum_poll();
    assert(hub_enum_cleanup_blocked() && !hub_enum_cleanup_pending());
    pending(&reply);
    assert(resumes == 2 && hub_enum_cleanup_blocked());
    hub_enum_parent_removed(5);
    begin(4);
    fail_requests();
    assert(hub_enum_cleanup.generation != old_generation);
    pending(&reply);
    assert(hub_enum_cleanup_pending());
    verify_disabled();
    assert(resumes == 3);

    begin(1);
    fail_requests();
    reply = completion(XFER_RESULT_SUCCESS, 0);
    hub_enum_parent_removed(5);
    pending(&reply);
    assert(!hub_enum_cleanup_pending() && !hub_enum_cleanup_blocked() && resumes == 3);
    begin(1);
    _dev0.hub_addr = 0;
    unsigned before = disables;
    fail_requests();
    assert(disables == before && resumes == 4 && !_dev0.enumerating);
    begin(1);
    submit_retry = false;
    tuh_xfer_t failed = {.result = XFER_RESULT_FAILED};
    process_enumeration(&failed);
    assert(hub_enum_cleanup_pending());
    verify_disabled();
    hub_usb_enum_status();
    assert(logs && hub_enum_cleanup.failure_count == 7);
    submit_retry = true;
    test_prepare();
    hub_usb_enum_status();
    puts("USB enumeration: real failure branch, disable/readback, per-port retries, errors, timeout and stale completion guards passed");
    return 0;
}
