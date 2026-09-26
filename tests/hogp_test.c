#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>

#include "..\src\hub_hogp.c"
#include "btstack_debug.h"
#include "gatt_values_under_test.inc"

static uint32_t now;
static bool ready[2], encrypted[2];
static uint8_t api_status[2];
static unsigned calls[2], listeners[2], scan_writes[2], done_events[2];
static uint16_t requested_uuid[2];
static uint8_t scan_value[2][4];

static unsigned index_for(uint16_t handle) { assert(handle == 0x40 || handle == 0x41); return handle - 0x40; }
uint32_t btstack_run_loop_get_time_ms(void) { return now; }
uint8_t gap_encryption_key_size(hci_con_handle_t h) { return encrypted[index_for(h)] ? 16 : 0; }
int gatt_client_is_ready(hci_con_handle_t h) { return ready[index_for(h)]; }
void hub_perf_input_phase(unsigned input, uint16_t h, hub_perf_input_phase_t phase, uint8_t status) {
    (void)status;
    assert(index_for(h) == input && phase == HUB_PERF_INPUT_HOGP_DONE);
    ++done_events[input];
}
void hci_dump_log(int level, const char *format, ...) { (void)level; (void)format; }
void btstack_assert_failed(const char *file, uint16_t line) { fprintf(stderr, "%s:%u\n", file, line); abort(); }
static uint8_t submitted(btstack_packet_handler_t cb, uint16_t h) {
    assert(cb == hogp_event); ++calls[index_for(h)]; return api_status[index_for(h)];
}
uint8_t gatt_client_discover_primary_services_by_uuid16(btstack_packet_handler_t cb, hci_con_handle_t h, uint16_t uuid) {
    requested_uuid[index_for(h)] = uuid; return submitted(cb, h);
}
uint8_t gatt_client_discover_characteristics_for_service(btstack_packet_handler_t cb, hci_con_handle_t h, gatt_client_service_t *s) {
    assert(s->start_group_handle && s->end_group_handle > s->start_group_handle);
    return submitted(cb, h);
}
uint8_t gatt_client_read_value_of_characteristic_using_value_handle(btstack_packet_handler_t cb, hci_con_handle_t h, uint16_t a) {
    assert(a == peers[index_for(h)].pnp.value_handle); return submitted(cb, h);
}
uint8_t gatt_client_read_value_of_characteristics_by_uuid16(btstack_packet_handler_t cb, hci_con_handle_t h,
        uint16_t start, uint16_t end, uint16_t uuid) {
    assert(start == 1 && end == 0xffff && uuid == ORG_BLUETOOTH_CHARACTERISTIC_HID_INFORMATION);
    return submitted(cb, h);
}
uint8_t gatt_client_write_client_characteristic_configuration(btstack_packet_handler_t cb, hci_con_handle_t h,
        gatt_client_characteristic_t *c, uint16_t config) {
    assert(c->value_handle == peers[index_for(h)].refresh.value_handle && config == 1);
    return submitted(cb, h);
}
void gatt_client_listen_for_characteristic_value_updates(gatt_client_notification_t *n, btstack_packet_handler_t cb,
        hci_con_handle_t h, gatt_client_characteristic_t *c) {
    unsigned i = index_for(h);
    assert(n == &peers[i].listener && cb == hogp_event && c == &peers[i].refresh);
    assert(!listeners[i]); ++listeners[i];
}
void gatt_client_stop_listening_for_characteristic_value_updates(gatt_client_notification_t *n) {
    for (unsigned i = 0; i < 2; ++i) {
        if (n != &peers[i].listener) continue;
        assert(listeners[i] == 1); --listeners[i]; return;
    }
    assert(false);
}
uint8_t gatt_client_write_value_of_characteristic_without_response(hci_con_handle_t h, uint16_t a, uint16_t length, uint8_t *value) {
    unsigned i = index_for(h);
    assert(a == peers[i].scan.value_handle && length == 4);
    ++calls[i];
    if (api_status[i]) return api_status[i];
    memcpy(scan_value[i], value, 4); ++scan_writes[i]; return 0;
}
static void reset(void) {
    hub_hogp_init();
    memset(calls, 0, sizeof(calls)); memset(listeners, 0, sizeof(listeners));
    memset(scan_writes, 0, sizeof(scan_writes)); memset(done_events, 0, sizeof(done_events));
    memset(api_status, 0, sizeof(api_status));
    now = 100;
    for (unsigned i = 0; i < 2; ++i) ready[i] = encrypted[i] = true;
    hub_hogp_start(0, 0x40);
}
static void emit(unsigned i, uint8_t *packet, uint16_t size) {
    packet[1] = (uint8_t)(size - 2);
    little_endian_store_16(packet, 2, (uint16_t)(0x40 + i));
    hogp_event(HCI_EVENT_PACKET, 0, packet, size);
}
static void complete(unsigned i, uint8_t status) {
    uint8_t packet[9] = {GATT_EVENT_QUERY_COMPLETE};
    packet[8] = status; emit(i, packet, sizeof(packet));
}
static void service(unsigned i, uint16_t uuid) {
    uint8_t packet[28] = {GATT_EVENT_SERVICE_QUERY_RESULT}, id[16];
    little_endian_store_16(packet, 8, 0x10); little_endian_store_16(packet, 10, 0x50);
    uuid_add_bluetooth_prefix(id, uuid); reverse_128(id, packet + 12);
    emit(i, packet, sizeof(packet));
}
static void characteristic(unsigned i, uint16_t uuid, uint16_t value_handle, uint16_t properties) {
    uint8_t packet[32] = {GATT_EVENT_CHARACTERISTIC_QUERY_RESULT}, id[16];
    little_endian_store_16(packet, 8, value_handle - 1); little_endian_store_16(packet, 10, value_handle);
    little_endian_store_16(packet, 12, value_handle + 3); little_endian_store_16(packet, 14, properties);
    uuid_add_bluetooth_prefix(id, uuid); reverse_128(id, packet + 16);
    emit(i, packet, sizeof(packet));
}
static void value(unsigned i, uint8_t event, uint16_t attribute, const uint8_t *data, uint16_t length) {
    uint8_t packet[40] = {0};
    assert(length <= sizeof(packet) - 12);
    packet[0] = event; little_endian_store_16(packet, 8, attribute); little_endian_store_16(packet, 10, length);
    memcpy(packet + 12, data, length); emit(i, packet, (uint16_t)(12 + length));
}
static void identify(unsigned i) {
    hub_hogp_tick();
    assert(requested_uuid[i] == ORG_BLUETOOTH_SERVICE_DEVICE_INFORMATION);
    service(i, ORG_BLUETOOTH_SERVICE_DEVICE_INFORMATION); complete(i, 0);
    hub_hogp_tick();
    characteristic(i, ORG_BLUETOOTH_CHARACTERISTIC_PNP_ID, 0x11, ATT_PROPERTY_READ);
    complete(i, 0); hub_hogp_tick();
    const uint8_t pnp[] = {2, 0x6d, 4, 0x15, 0xb0, 0, 1};
    value(i, GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT, 0x11, pnp, sizeof(pnp));
    complete(i, 0); hub_hogp_tick();
    const uint8_t info[] = {0x11, 1, 0, 3};
    value(i, GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT, 0x70, info, sizeof(info));
    complete(i, 0);
    assert(peers[i].pnp_valid && peers[i].vendor == 0x046d && peers[i].product == 0xb015);
    assert(peers[i].info_count == 1 && peers[i].info_flags[0] == 3 && peers[i].phase == HOGP_SCAN_SERVICE);
}
static void scan_service(unsigned i, bool refresh) {
    hub_hogp_tick(); service(i, ORG_BLUETOOTH_SERVICE_SCAN_PARAMETERS); complete(i, 0);
    hub_hogp_tick();
    characteristic(i, ORG_BLUETOOTH_CHARACTERISTIC_SCAN_INTERVAL_WINDOW, 0x21, ATT_PROPERTY_WRITE_WITHOUT_RESPONSE);
    if (refresh) characteristic(i, ORG_BLUETOOTH_CHARACTERISTIC_SCAN_REFRESH, 0x31, ATT_PROPERTY_NOTIFY);
    complete(i, 0);
    if (refresh) {
        hub_hogp_tick(); assert(listeners[i] == 1 && peers[i].pending);
        complete(i, 0);
    }
    hub_hogp_tick();
    assert(peers[i].phase == HOGP_DONE && scan_writes[i] == 1 && !peers[i].pending);
    assert(little_endian_read_16(scan_value[i], 0) == 0x60 && little_endian_read_16(scan_value[i], 2) == 0x30);
}
static void test_optional_scan_and_refresh(void) {
    reset(); identify(0); hub_hogp_tick(); complete(0, 0);
    assert(peers[0].phase == HOGP_DONE && !peers[0].degraded && peers[0].scan_checked && !peers[0].scan_present);
    assert(done_events[0] == 1 && !scan_writes[0]);
    reset(); identify(0); scan_service(0, false);
    assert(!listeners[0] && !peers[0].scan_subscribed);
    reset(); identify(0); scan_service(0, true);
    assert(peers[0].scan_subscribed);
    const uint8_t refresh = 0;
    value(0, GATT_EVENT_NOTIFICATION, 0x31, &refresh, 1);
    value(0, GATT_EVENT_NOTIFICATION, 0x31, &refresh, 1);
    api_status[0] = GATT_CLIENT_BUSY;
    hub_hogp_tick();
    assert(scan_writes[0] == 1 && !peers[0].degraded && !peers[0].pending && peers[0].refresh_pending);
    api_status[0] = 0; hub_hogp_tick();
    assert(scan_writes[0] == 2 && peers[0].refreshes == 2 && !peers[0].refresh_pending);
    uint8_t invalid = 1;
    value(0, GATT_EVENT_NOTIFICATION, 0x31, &invalid, 1);
    value(0, GATT_EVENT_NOTIFICATION, 0x99, &refresh, 1);
    hub_hogp_tick(); assert(scan_writes[0] == 2);
    hub_hogp_stop(0);
    value(0, GATT_EVENT_NOTIFICATION, 0x31, &refresh, 1);
    hub_hogp_tick();
    assert(!listeners[0] && !peers[0].active && scan_writes[0] == 2);
}
static void test_failures_and_timeout(void) {
    reset(); hub_hogp_tick(); complete(0, ATT_ERROR_INSUFFICIENT_ENCRYPTION);
    assert(peers[0].degraded && peers[0].phase == HOGP_HID_INFO && !peers[0].pnp_valid);
    hub_hogp_tick(); complete(0, ATT_ERROR_ATTRIBUTE_NOT_FOUND);
    assert(peers[0].phase == HOGP_SCAN_SERVICE);
    hub_hogp_tick(); complete(0, 0);
    assert(peers[0].phase == HOGP_DONE && peers[0].errors == 2);
    reset(); identify(0);
    hub_hogp_tick(); service(0, ORG_BLUETOOTH_SERVICE_SCAN_PARAMETERS); complete(0, 0);
    hub_hogp_tick();
    characteristic(0, ORG_BLUETOOTH_CHARACTERISTIC_SCAN_INTERVAL_WINDOW, 0x21, ATT_PROPERTY_WRITE_WITHOUT_RESPONSE);
    characteristic(0, ORG_BLUETOOTH_CHARACTERISTIC_SCAN_REFRESH, 0x31, ATT_PROPERTY_NOTIFY);
    complete(0, 0); hub_hogp_tick(); complete(0, ATT_ERROR_WRITE_NOT_PERMITTED);
    assert(peers[0].degraded && !peers[0].scan_subscribed && !listeners[0] && !scan_writes[0]);
    reset(); api_status[0] = ERROR_CODE_MEMORY_CAPACITY_EXCEEDED; hub_hogp_tick();
    assert(peers[0].degraded && peers[0].phase == HOGP_HID_INFO && !peers[0].pending);
    reset(); hub_hogp_tick(); now += HOGP_PHASE_TIMEOUT_MS; hub_hogp_tick();
    unsigned before = calls[0];
    assert(peers[0].phase == HOGP_DONE && !peers[0].pending && peers[0].degraded);
    complete(0, 0); hub_hogp_tick(); assert(calls[0] == before);
    reset(); now = UINT32_MAX - 20; hub_hogp_start(0, 0x40);
    ready[0] = false; now += 25; hub_hogp_tick(); assert(!peers[0].degraded);
    now += HOGP_PHASE_TIMEOUT_MS; hub_hogp_tick(); assert(peers[0].degraded);
}
static void test_two_peers_and_invalid_values(void) {
    reset(); hub_hogp_start(1, 0x41);
    ready[1] = false; identify(0);
    assert(!calls[1] && peers[1].phase == HOGP_DIS_SERVICE);
    ready[1] = true; ready[0] = false; identify(1);
    ready[0] = true; hub_hogp_tick(); complete(0, 0); complete(1, 0);
    assert(done_events[0] == 1 && done_events[1] == 1);
    hub_hogp_stop(0); assert(peers[1].active);
    reset(); hub_hogp_tick(); service(0, ORG_BLUETOOTH_SERVICE_DEVICE_INFORMATION); complete(0, 0);
    hub_hogp_tick(); characteristic(0, ORG_BLUETOOTH_CHARACTERISTIC_PNP_ID, 0x11, ATT_PROPERTY_READ);
    complete(0, 0); hub_hogp_tick();
    const uint8_t malformed[] = {0, 1, 2, 3, 4, 5, 6};
    value(0, GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT, 0x11, malformed, sizeof(malformed));
    complete(0, 0);
    assert(!peers[0].pnp_valid && peers[0].degraded);
    reset(); hub_hogp_tick(); service(0, ORG_BLUETOOTH_SERVICE_DEVICE_INFORMATION);
    service(0, ORG_BLUETOOTH_SERVICE_DEVICE_INFORMATION); complete(0, 0);
    assert(peers[0].degraded);
    reset(); encrypted[0] = false; hub_hogp_tick();
    assert(peers[0].degraded && peers[0].phase == HOGP_DONE && !calls[0]);
}
int main(void) {
    test_optional_scan_and_refresh();
    test_failures_and_timeout();
    test_two_peers_and_invalid_values();
    puts("HOGP companion: discovery, refresh, errors, deadlines, privacy and two-peer isolation passed.");
    return 0;
}
