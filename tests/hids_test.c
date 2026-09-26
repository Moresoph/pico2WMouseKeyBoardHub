#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>

/* Actual generated SDK implementation; only GATT transport and allocation are mocked. */
#include "hids_host.c"
#include "gatt_values_under_test.inc"

static hids_host_t pool;
static bool allocated;
static unsigned frees, events, writes, listeners, trace_submits, trace_completions;
static uint8_t emitted_status, submit_status, scheduler_status, last_att;
static uint16_t last_report;
static uint8_t storage[256];
static gatt_client_notification_t *listening[HIDS_HOST_NUM_REPORTS];
static bool defer_query;
static btstack_context_callback_registration_t *queued_query;

void btstack_assert_failed(const char *file, uint16_t line) {
    fprintf(stderr, "%s:%u\n", file, line); abort();
}
void hci_dump_log(int level, const char *format, ...) { (void)level; (void)format; }
void hci_add_event_handler(btstack_packet_callback_registration_t *handler) { (void)handler; }
hids_host_t *btstack_memory_hids_host_get(void) {
    assert(!allocated); allocated = true; memset(&pool, 0, sizeof(pool)); return &pool;
}
void btstack_memory_hids_host_free(hids_host_t *client) {
    assert(client == &pool && allocated && listeners == 0);
    assert(queued_query != &client->gatt_query_request);
    allocated = false; ++frees; memset(client, 0xdd, sizeof(*client));
}
void hub_perf_hids_cccd(uint16_t handle, uint16_t attribute, bool completed, uint8_t status) {
    assert(handle == 0x41);
    last_report = attribute;
    if (completed) { ++trace_completions; last_att = status; }
    else ++trace_submits;
}
void gatt_client_listen_for_characteristic_value_updates(gatt_client_notification_t *n,
        btstack_packet_handler_t cb, hci_con_handle_t h, gatt_client_characteristic_t *c) {
    (void)cb; (void)h; (void)c;
    assert(listeners < HIDS_HOST_NUM_REPORTS); listening[listeners++] = n;
}
void gatt_client_stop_listening_for_characteristic_value_updates(gatt_client_notification_t *n) {
    for (unsigned i = 0; i < listeners; ++i) {
        if (listening[i] != n) continue;
        listening[i] = listening[--listeners]; return;
    }
}
uint8_t gatt_client_request_to_send_gatt_query(btstack_context_callback_registration_t *cb, hci_con_handle_t h) {
    (void)h;
    if (scheduler_status) return scheduler_status;
    if (queued_query == cb) return ERROR_CODE_COMMAND_DISALLOWED;
    if (defer_query) queued_query = cb;
    else cb->callback(cb->context);
    return 0;
}
uint8_t gatt_client_remove_gatt_query(btstack_context_callback_registration_t *cb, hci_con_handle_t h) {
    (void)h;
    if (queued_query == cb) queued_query = NULL;
    return 0;
}
uint8_t gatt_client_request_to_write_without_response(btstack_context_callback_registration_t *cb, hci_con_handle_t h) {
    (void)cb; (void)h; return ERROR_CODE_COMMAND_DISALLOWED;
}
uint8_t gatt_client_write_client_characteristic_configuration(btstack_packet_handler_t cb, hci_con_handle_t h,
        gatt_client_characteristic_t *c, uint16_t configuration) {
    (void)cb; assert(h == 0x41 && c->value_handle && configuration == 1); ++writes; return submit_status;
}
uint8_t gatt_client_get_mtu(hci_con_handle_t h, uint16_t *mtu) { (void)h; *mtu = 23; return 0; }
uint8_t gatt_client_discover_primary_services_by_uuid16(btstack_packet_handler_t cb, hci_con_handle_t h, uint16_t uuid) {
    (void)cb; (void)h; (void)uuid; return submit_status;
}
uint8_t gatt_client_discover_characteristics_for_service(btstack_packet_handler_t cb, hci_con_handle_t h, gatt_client_service_t *s) {
    (void)cb; (void)h; (void)s; return submit_status;
}
uint8_t gatt_client_discover_characteristic_descriptors(btstack_packet_handler_t cb, hci_con_handle_t h,
        gatt_client_characteristic_t *c) {
    (void)cb; (void)h; (void)c; return submit_status;
}
uint8_t gatt_client_read_long_value_of_characteristic_using_value_handle(btstack_packet_handler_t cb, hci_con_handle_t h, uint16_t a) {
    (void)cb; (void)h; (void)a; return submit_status;
}
uint8_t gatt_client_read_characteristic_descriptor_using_descriptor_handle(btstack_packet_handler_t cb, hci_con_handle_t h, uint16_t a) {
    (void)cb; (void)h; (void)a; return submit_status;
}
uint8_t gatt_client_read_value_of_characteristic_using_value_handle(btstack_packet_handler_t cb, hci_con_handle_t h, uint16_t a) {
    (void)cb; (void)h; (void)a; return submit_status;
}
uint8_t gatt_client_write_value_of_characteristic(btstack_packet_handler_t cb, hci_con_handle_t h,
        uint16_t a, uint16_t length, uint8_t *value) {
    (void)cb; (void)h; (void)a; (void)length; (void)value; return submit_status;
}
uint8_t gatt_client_write_value_of_characteristic_without_response(hci_con_handle_t h, uint16_t a, uint16_t length, uint8_t *value) {
    (void)h; (void)a; (void)length; (void)value; return submit_status;
}
static void application(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    assert(type == HCI_EVENT_PACKET && size == 8 && packet[2] == GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED);
    ++events; emitted_status = packet[5];
}
static hids_host_t *setup(unsigned reports) {
    if (allocated) hids_host_finalize(&pool);
    assert(!queued_query);
    defer_query = false;
    hids_host_init(storage, sizeof(storage));
    frees = events = writes = listeners = trace_submits = trace_completions = 0;
    submit_status = scheduler_status = last_att = 0;
    emitted_status = 0xff;
    hids_host_t *client = hids_host_create(0x41, 7);
    client->client_handler = application;
    client->num_instances = 1;
    client->services[0].protocol_mode = HID_PROTOCOL_MODE_REPORT;
    client->num_reports = (uint8_t)reports;
    client->state = HIDS_HOST_STATE_W2_ENABLE_INPUT_REPORTS;
    for (unsigned i = 0; i < reports; ++i) {
        client->reports[i].report_type = HID_REPORT_TYPE_INPUT;
        client->reports[i].value_handle = (uint16_t)(0x30 + i * 0x10);
        client->reports[i].end_handle = client->reports[i].value_handle + 3;
        client->reports[i].properties = ATT_PROPERTY_NOTIFY;
    }
    return client;
}
static void complete(uint8_t status) {
    uint8_t packet[9] = {GATT_EVENT_QUERY_COMPLETE, 7, 0x41, 0, 0, 0, 0, 0, 0};
    packet[8] = status;
    handle_gatt_client_event(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
}
static void test_success_and_every_failed_cccd(void) {
    hids_host_t *client = setup(2);
    hids_host_request_to_send_next_query(client);
    assert(writes == 1 && listeners == 1 && !events);
    complete(0);
    assert(writes == 2 && listeners == 2 && !events && last_report == 0x40);
    complete(0);
    assert(events == 1 && !emitted_status && allocated && !frees && client->state == HIDS_HOST_STATE_CONNECTED);
    assert(trace_submits == 2 && trace_completions == 2);
    for (unsigned failed = 0; failed < 2; ++failed) {
        for (unsigned status = 1; status <= 0xff; ++status) {
            client = setup(2);
            hids_host_request_to_send_next_query(client);
            if (failed) complete(0);
            complete((uint8_t)status);
            assert(events == 1 && emitted_status != 0 && !allocated && frees == 1 && !listeners);
            assert(last_att == status && trace_completions == failed + 1 && writes == failed + 1);
        }
    }
}
static void test_immediate_and_scheduler_failures(void) {
    for (unsigned failed = 0; failed < 2; ++failed) {
        hids_host_t *client = setup(2);
        if (failed) hids_host_request_to_send_next_query(client);
        submit_status = GATT_CLIENT_CHARACTERISTIC_NOTIFICATION_NOT_SUPPORTED;
        if (failed) complete(0);
        else hids_host_request_to_send_next_query(client);
        assert(events == 1 && emitted_status == submit_status && frees == 1 && !allocated && !listeners);
    }
    hids_host_t *client = setup(1);
    scheduler_status = ERROR_CODE_MEMORY_CAPACITY_EXCEEDED;
    hids_host_request_to_send_next_query(client);
    assert(events == 1 && emitted_status == scheduler_status && !writes && !allocated);
    client = setup(1);
    client->state = HIDS_HOST_STATE_W2_READ_REPORT_MAP_HID_DESCRIPTOR;
    submit_status = ERROR_CODE_MEMORY_CAPACITY_EXCEEDED;
    hids_host_request_to_send_next_query(client);
    assert(events == 1 && emitted_status == submit_status && !allocated);
}
static void test_reference_and_discovery_failure(void) {
    const hid_service_client_state_t states[] = {
        HIDS_HOST_STATE_W4_REPORT_MAP_CHARACTERISTIC_DESCRIPTORS_RESULT,
        HIDS_HOST_STATE_W4_REPORT_MAP_EXTERNAL_REPORT_REFERENCE_UUID,
        HIDS_HOST_STATE_W4_EXTERNAL_REPORT_CHARACTERISTIC_RESULT,
        HIDS_HOST_STATE_W4_REPORT_FOUND, HIDS_HOST_STATE_W4_REPORT_ID_AND_TYPE
    };
    for (unsigned i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        hids_host_t *client = setup(1);
        client->state = states[i]; complete(ATT_ERROR_INSUFFICIENT_ENCRYPTION);
        assert(events == 1 && emitted_status && !allocated);
    }
    hids_host_t *client = setup(1);
    client->state = HIDS_HOST_STATE_W4_REPORT_FOUND;
    client->handle = 0; complete(0);
    assert(emitted_status && !allocated && !writes);
    client = setup(1);
    client->state = HIDS_HOST_STATE_W4_REPORT_ID_AND_TYPE;
    client->reports[0].report_type = 0; complete(0);
    assert(emitted_status && !allocated && !writes);
}
static void test_queued_result_callbacks(void) {
    hids_host_t *client = setup(2);
    hids_host_request_to_send_next_query(client);
    defer_query = true;
    hids_host_request_to_send_next_query(client);
    hids_host_request_to_send_next_query(client);
    assert(allocated && !events && queued_query && writes == 1);
    complete(0);
    assert(allocated && !events && writes == 1 && queued_query);
    btstack_context_callback_registration_t *cb = queued_query;
    queued_query = NULL;
    defer_query = false;
    cb->callback(cb->context);
    assert(writes == 2 && !events);
    complete(0);
    assert(events == 1 && emitted_status == 0);

    client = setup(1);
    hids_host_request_to_send_next_query(client);
    defer_query = true;
    hids_host_request_to_send_next_query(client);
    assert(queued_query);
    complete(ATT_ERROR_INSUFFICIENT_ENCRYPTION);
    assert(!allocated && !queued_query && frees == 1 && emitted_status != 0);
}
int main(void) {
    test_queued_result_callbacks();
    test_success_and_every_failed_cccd();
    test_immediate_and_scheduler_failures();
    test_reference_and_discovery_failure();
    puts("Pinned HIDS: subscription failure propagation, cleanup, prerequisites and success passed.");
    return 0;
}
