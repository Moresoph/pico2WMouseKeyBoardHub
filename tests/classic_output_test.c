#include <assert.h>
#include <limits.h>
#include <stdlib.h>

#include "..\src\hub_output_store.c"
#include "..\src\hub_classic_output.c"

static const bd_addr_t pc = {0x20, 1, 2, 3, 4, 5};
static const bd_addr_t stranger = {0x22, 1, 2, 3, 4, 6};
static const uint8_t key_down[8] = {0, 0, 4};
static uint8_t flash[HUB_OUTPUT_STORE_BYTES];
static uint8_t ble_flash[HUB_OUTPUT_STORE_BYTES];
static int flash_length;
static unsigned ble_reads, ble_writes;
static unsigned writes, write_attempts, router_releases, security_requests, disconnects;
static unsigned report_count, perf_samples, request_count, callback_depth, max_depth;
static unsigned bond_reads;
static unsigned att_initializations, hid_initializations;
static int free_acl_slots;
static bool packet_buffer_reserved, transport_available;
static bool write_error, read_error, readback_error, no_tlv, pairing_open, input_collision, input_busy;
static bool have_bond, channel_ready, sync_send, pending_send, grant, timer_pending;
static bool boot_mode, mux_pending;
static uint8_t encryption_size, send_error, sdp_error;
static link_key_type_t key_type;
static gap_security_level_t security_level;
static uint16_t mock_handle, mock_cid;
static uint32_t clock_ms, clock_us;
static uint32_t timer_due_us;
static bool timer_error;
static struct { uint8_t data[10]; unsigned size; uint32_t at_us; } reports[1024];

static int tlv_get(void *context, uint32_t tag, uint8_t *data, uint32_t size) {
    (void)context;
    if (tag == 0x50484f31u) {
        ++ble_reads;
        if (data) memcpy(data, ble_flash, size < sizeof(ble_flash) ? size : sizeof(ble_flash));
        return sizeof(ble_flash);
    }
    assert(tag == 0x50484331u);
    if (read_error) return -1;
    if (data && flash_length > 0) {
        memcpy(data, flash, size < (unsigned)flash_length ? size : (unsigned)flash_length);
        if (readback_error && writes) data[0] ^= 1;
    }
    return flash_length;
}
static int tlv_store(void *context, uint32_t tag, const uint8_t *data, uint32_t size) {
    (void)context;
    if (tag == 0x50484f31u) {
        assert(size == sizeof(ble_flash));
        ++ble_writes;
        memcpy(ble_flash, data, size);
        return 0;
    }
    assert(tag == 0x50484331u && size == sizeof(flash));
    ++write_attempts;
    if (write_error) return -1;
    ++writes;
    memcpy(flash, data, size);
    flash_length = (int)size;
    return 0;
}
static void tlv_delete(void *context, uint32_t tag) {
    (void)context; (void)tag;
    assert(!"Output must never erase any identity or bond");
}
static const btstack_tlv_t tlv = {tlv_get, tlv_store, tlv_delete};
void btstack_tlv_get_instance(const btstack_tlv_t **impl, void **context) {
    *impl = no_tlv ? NULL : &tlv;
    *context = NULL;
}
uint32_t btstack_run_loop_get_time_ms(void) { return clock_ms; }
uint32_t hub_latency_now_us(void) { return clock_us; }
bool hub_security_pairing_open(void) { return pairing_open; }
bool hub_input_is_classic_target(const uint8_t address[6]) {
    return input_collision && !memcmp(address, pc, 6);
}
bool hub_input_radio_busy(void) { return input_busy; }
void hub_router_release_all(void) { ++router_releases; hub_output_release_all(); }
void hub_perf_output_wait(uint16_t h, unsigned slot, bool mouse, uint32_t wait) {
    (void)mouse; (void)wait;
    assert(h == mock_handle && slot == 0 && verified);
    ++perf_samples;
}
void btstack_run_loop_set_timer_handler(btstack_timer_source_t *timer,
                                        void (*callback)(btstack_timer_source_t *)) {
    timer->process = callback;
}
void btstack_run_loop_set_timer(btstack_timer_source_t *timer, uint32_t delay) {
    assert(timer == &service_timer && delay == SERVICE_MS);
}
void btstack_run_loop_add_timer(btstack_timer_source_t *timer) {
    assert(timer == &service_timer);
}
bool hub_output_timer_schedule_us(uint32_t delay, void (*callback)(void)) {
    assert(!timer_pending && delay > 0 && delay <= SEND_PERIOD_US);
    assert(callback == pump_after_yield);
    if (timer_error) return false;
    timer_due_us = clock_us + delay;
    timer_pending = true;
    return true;
}
void hci_add_event_handler(btstack_packet_callback_registration_t *registration) {
    assert(registration->callback == hci_event);
}
bool gap_get_link_key_for_bd_addr(bd_addr_t address, link_key_t key, link_key_type_t *type) {
    assert(!memcmp(address, pc, 6) || !have_bond);
    ++bond_reads;
    memset(key, 0x33, sizeof(link_key_t));
    *type = key_type;
    return have_bond;
}
uint8_t gap_encryption_key_size(hci_con_handle_t h) {
    return h == mock_handle ? encryption_size : 0;
}
gap_security_level_t gap_security_level(hci_con_handle_t h) {
    return h == mock_handle ? security_level : LEVEL_0;
}
void gap_request_security_level(hci_con_handle_t h, gap_security_level_t level) {
    assert(h == mock_handle && level == LEVEL_2);
    ++security_requests;
    if (security_level >= LEVEL_2) {
        uint8_t event[] = {GAP_EVENT_SECURITY_LEVEL, 4, 0, 0, LEVEL_2, 0};
        little_endian_store_16(event, 2, h);
        hci_event(HCI_EVENT_PACKET, 0, event, sizeof(event));
    }
}
uint8_t gap_disconnect(hci_con_handle_t h) {
    assert(h == mock_handle);
    ++disconnects;
    return 0;
}
int hci_number_free_acl_slots_for_handle(hci_con_handle_t h) {
    assert(h == mock_handle);
    return free_acl_slots;
}
bool hci_is_packet_buffer_reserved(void) { return packet_buffer_reserved; }
bool hci_can_send_prepared_acl_packet_now(hci_con_handle_t h) {
    assert(h == mock_handle);
    return free_acl_slots > 0 && transport_available;
}
void gap_connectable_control(uint8_t enabled) { assert(enabled == 1); }
void gap_discoverable_control(uint8_t enabled) {
    if (enabled) assert(pairing_open && selected && started);
}
void hid_device_init(bool boot, uint16_t size, const uint8_t *descriptor) {
    assert(att_initializations == ++hid_initializations);
    assert(!boot && size == sizeof(hub_hid_report_descriptor) && descriptor == hub_hid_report_descriptor);
}
void att_server_init(const uint8_t *db, att_read_callback_t read_callback,
                     att_write_callback_t write_callback) {
    assert(db == profile_data && !read_callback && !write_callback);
    ++att_initializations;
}
void hid_device_accept_truncated_hid_reports(bool accept) { assert(!accept); }
void hid_device_register_packet_handler(btstack_packet_handler_t callback) { assert(callback == hid_event); }
void hid_device_register_report_request_callback(int (*callback)(uint16_t, hid_report_type_t, uint16_t, int *, uint8_t *)) {
    assert(callback == get_report);
}
void hid_device_register_set_report_callback(void (*callback)(uint16_t, hid_report_type_t, int, uint8_t *)) {
    assert(callback == set_report);
}
void hid_device_register_report_data_callback(void (*callback)(uint16_t, hid_report_type_t, uint16_t, int, uint8_t *)) {
    assert(callback == report_data);
}
void hid_create_sdp_record(uint8_t *record, uint32_t record_handle, const hid_sdp_record_t *params) {
    (void)record_handle;
    assert(record == sdp_record && !params->hid_boot_device && !params->hid_reconnect_initiate);
    assert(params->hid_descriptor == hub_hid_report_descriptor);
}
uint8_t sdp_register_service(const uint8_t *record) { assert(record == sdp_record); return sdp_error; }
uint8_t hid_device_connect(bd_addr_t address, uint16_t *cid) {
    (void)address; (void)cid;
    assert(!"Incoming-only backend must never reset the stock singleton with connect");
    return ERROR_CODE_COMMAND_DISALLOWED;
}
void hid_device_disconnect(uint16_t cid) { assert(cid == mock_cid); }
int hid_device_in_boot_protocol_mode(uint16_t cid) { assert(cid == mock_cid); return boot_mode; }
bool hub_classic_mux_output_ready(uint16_t h) { return channel_ready && h == mock_handle; }
bool hub_classic_mux_output_pending(void) { return mux_pending; }
uint8_t hub_classic_mux_send(const uint8_t *packet, uint16_t size) {
    assert(grant && channel_ready && verified && encryption_size == 16);
    assert(packet[0] == 0xa1 && (packet[1] == 1 || packet[1] == 2));
    assert(size == (packet[1] == 1 ? 10 : 8));
    grant = false;
    if (send_error) return send_error;
    assert(report_count < sizeof(reports) / sizeof(reports[0]));
    memcpy(reports[report_count].data, packet, size);
    reports[report_count].at_us = clock_us;
    reports[report_count++].size = size;
    return 0;
}
static void emit_hid(uint8_t subevent, uint16_t cid) {
    uint8_t event[] = {HCI_EVENT_HID_META, 3, 0, 0, 0};
    event[2] = subevent;
    little_endian_store_16(event, 3, cid);
    hid_event(HCI_EVENT_PACKET, cid, event, sizeof(event));
}
static void send_credit(void) {
    pending_send = false;
    grant = true;
    ++callback_depth;
    if (callback_depth > max_depth) max_depth = callback_depth;
    assert(callback_depth == 1);
    emit_hid(HID_SUBEVENT_CAN_SEND_NOW, mock_cid);
    --callback_depth;
}
void hid_device_request_can_send_now_event(uint16_t cid) {
    assert(cid == mock_cid);
    ++request_count;
    if (sync_send) send_credit();
    else pending_send = true;
}
static void tick(void) {
    if (timer_pending) {
        timer_pending = false;
        uint32_t elapsed = timer_due_us - clock_us;
        clock_us = timer_due_us;
        clock_ms += (elapsed + 999u) / 1000u;
        pump_after_yield();
    } else service(&service_timer);
}
static void drain(void) {
    for (unsigned i = 0; i < 1024 && (count || releases || timer_pending); ++i) {
        if (pending_send) send_credit();
        else tick();
        if (!transport_ready()) break;
    }
    assert(!count && !releases);
}
static void reset(void) {
    hub_output_store_t ble_store = {0};
    ble_store.identities[0].reserved = true;
    memcpy(ble_store.identities[0].address, stranger, 6);
    ble_store.identities[1].reserved = true;
    memcpy(ble_store.identities[1].address, pc, 6);
    ble_store.selected = 1;
    encode(&ble_store, ble_flash);
    memset(flash, 0, sizeof(flash));
    flash_length = 0;
    ble_reads = ble_writes = 0;
    writes = write_attempts = router_releases = security_requests = disconnects = 0;
    report_count = perf_samples = request_count = callback_depth = max_depth = 0;
    bond_reads = 0;
    att_initializations = hid_initializations = 0;
    free_acl_slots = 3;
    packet_buffer_reserved = false;
    transport_available = true;
    write_error = read_error = readback_error = no_tlv = pairing_open = input_collision = input_busy = false;
    have_bond = channel_ready = sync_send = pending_send = grant = timer_pending = boot_mode = mux_pending = false;
    encryption_size = send_error = sdp_error = 0;
    timer_error = false;
    security_level = LEVEL_0;
    key_type = UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P256;
    mock_handle = 0x31;
    mock_cid = 1;
    clock_ms = clock_us = 0;
    hub_output_init();
    hub_output_start();
}
static void connection_complete(void) {
    uint8_t event[13] = {HCI_EVENT_CONNECTION_COMPLETE, 11};
    little_endian_store_16(event, 3, mock_handle);
    reverse_bd_addr(pc, &event[5]);
    event[11] = 1;
    hci_event(HCI_EVENT_PACKET, 0, event, sizeof(event));
}
static void hid_open(void) {
    uint8_t event[15] = {HCI_EVENT_HID_META, 13, HID_SUBEVENT_CONNECTION_OPENED};
    little_endian_store_16(event, 3, mock_cid);
    reverse_bd_addr(pc, &event[6]);
    little_endian_store_16(event, 12, mock_handle);
    channel_ready = true;
    hid_event(HCI_EVENT_PACKET, mock_cid, event, sizeof(event));
}
static void encrypt(void) {
    have_bond = true;
    encryption_size = 16;
    security_level = LEVEL_2;
    uint8_t event[] = {GAP_EVENT_SECURITY_LEVEL, 4, 0, 0, LEVEL_2, 0};
    little_endian_store_16(event, 2, mock_handle);
    hci_event(HCI_EVENT_PACKET, 0, event, sizeof(event));
}
static void ready(void) {
    assert(hub_classic_output_set_target(pc));
    pairing_open = true;
    assert(hub_classic_output_accept_connection(pc));
    connection_complete();
    hid_open();
    assert(!hub_output_ready(0) && !writes && !report_count);
    encrypt();
    drain();
    assert(hub_output_ready(0) && writes == 1 && security_requests >= 1);
    assert(report_count == 2 && reports[0].data[1] == 1 && reports[1].data[1] == 2);
}
static void disconnected(uint16_t h) {
    uint8_t event[] = {HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0, 0, 0, 0x13};
    little_endian_store_16(event, 3, h);
    hci_event(HCI_EVENT_PACKET, 0, event, sizeof(event));
}

static void test_authorization(void) {
    reset();
    assert(!hub_classic_output_accept_connection(pc) && !hub_classic_output_connect());
    assert(!hub_classic_output_set_target(NULL));
    const uint8_t zero[6] = {0};
    assert(!hub_classic_output_set_target(zero));
    input_collision = true;
    assert(!hub_classic_output_set_target(pc));
    input_collision = false;
    assert(hub_classic_output_set_target(pc) && !writes);
    assert(!hub_classic_output_allow_pairing(pc));
    assert(!hub_classic_output_accept_connection(pc));
    pairing_open = true;
    assert(!hub_classic_output_accept_connection(stranger));
    assert(!hub_classic_output_allow_pairing(stranger));
    assert(hub_classic_output_allow_pairing(pc));
    pairing_open = false;
    assert(!hub_classic_output_accept_connection(pc)); /* Even before main notices expiry. */
    hub_classic_output_pairing_changed(false);
    assert(!hub_classic_output_accept_connection(pc));
    pairing_open = true;
    input_busy = true;
    assert(!hub_classic_output_connect());
    input_busy = false;
    assert(!hub_classic_output_connect() && !hid_cid);
    assert(hub_classic_output_accept_connection(pc));
    assert(!hub_classic_output_set_target(stranger));
    assert(!hub_classic_output_connect());
    connection_complete();
    assert(hub_classic_output_accept_connection(pc));
    hid_open();
    uint8_t level_zero[] = {GAP_EVENT_SECURITY_LEVEL, 4, 0, 0, LEVEL_0, 0};
    little_endian_store_16(level_zero, 2, mock_handle);
    hci_event(HCI_EVENT_PACKET, 0, level_zero, sizeof(level_zero));
    assert(!disconnecting); /* Global Level0 L2CAP policy must not abort Level2 setup. */
    encrypt();
    drain();
    assert(writes == 1 && hub_output_ready(0));
    pairing_open = false;
    hub_classic_output_pairing_changed(false);
    assert(!hub_classic_output_allow_pairing(pc));
    assert(hub_output_ready(0) && !disconnecting);
    assert(!hub_output_ready(1) && !hub_output_select(1));
    assert(hub_output_select(0) && hub_output_active_slot() == 0 && !hub_output_switching());
    input_collision = true;
    assert(!hub_classic_output_is_peer(pc) && !hub_output_ready(0));
    service(&service_timer);
    assert(disconnecting && router_releases);
}

static void test_storage_security(void) {
    reset();
    hub_classic_output_set_target(pc);
    pairing_open = true;
    hub_classic_output_accept_connection(pc);
    connection_complete();
    hid_open();
    encryption_size = 16;
    security_level = LEVEL_2;
    update_security();
    assert(!writes && !hub_output_ready(0));
    have_bond = true;
    key_type = DEBUG_COMBINATION_KEY;
    update_security();
    assert(!writes);
    key_type = UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P256;
    encryption_size = 15;
    update_security();
    assert(!writes);
    encryption_size = 16;
    write_error = true;
    update_security();
    assert(store.fault && !verified && disconnecting && !writes && write_attempts == 1);
    assert(!hub_classic_output_set_target(stranger));

    reset();
    hub_output_store_t wrong;
    assert(hub_output_store_load(&wrong));
    assert(hub_output_store_claim(&wrong, 0, pc) == 0);
    assert(hub_output_store_claim(&wrong, 0, stranger) == 1);
    hub_output_init();
    hub_output_start();
    assert(store.fault && !hub_classic_output_set_target(pc));

    reset();
    flash_length = sizeof(flash);
    hub_output_init();
    hub_output_start();
    assert(store.fault && !hub_classic_output_accept_connection(pc));
    reset();
    no_tlv = true;
    hub_output_init();
    hub_output_start();
    assert(store.fault);
    reset();
    readback_error = true;
    hub_classic_output_set_target(pc);
    pairing_open = true;
    hub_classic_output_accept_connection(pc);
    connection_complete();
    hid_open();
    encrypt();
    assert(writes == 1 && store.fault && !verified && disconnecting);

    reset();
    assert(hub_output_store_load(&wrong));
    const uint8_t random_address[6] = {0xc2, 1, 2, 3, 4, 5};
    assert(hub_output_store_claim(&wrong, 1, random_address) == 0);
    hub_output_init();
    hub_output_start();
    assert(store.fault && !hub_classic_output_accept_connection(random_address));
}

static void test_storage_namespace(void) {
    reset();
    assert(HUB_OUTPUT_STORE_TAG == 0x50484331u);
    assert(HUB_OUTPUT_STORE_TAG != 0x50484f31u);
    uint8_t original_ble[HUB_OUTPUT_STORE_BYTES];
    memcpy(original_ble, ble_flash, sizeof(original_ble));
    ready();
    assert(store.identities[0].reserved && store.identities[0].type == 0);
    for (unsigned i = 1; i < HUB_OUTPUT_STORE_SLOTS; ++i)
        assert(!store.identities[i].reserved);
    assert(!ble_reads && !ble_writes && !memcmp(original_ble, ble_flash, sizeof(original_ble)));
    hub_output_init();
    hub_output_start();
    assert(saved() && store.identities[0].type == 0 && writes == 1);
    assert(!ble_reads && !ble_writes && !memcmp(original_ble, ble_flash, sizeof(original_ble)));
    hub_output_store_t preserved_ble;
    assert(decode(&preserved_ble, ble_flash) && preserved_ble.selected == 1);
}

static void test_reconnect_stale(void) {
    reset();
    ready();
    pairing_open = false;
    hub_classic_output_pairing_changed(false);
    assert(!disconnecting && !lease && writes == 1);
    uint16_t previous_handle = mock_handle, previous_cid = mock_cid;
    channel_ready = false;
    disconnected(previous_handle);
    assert(!hub_output_ready(0) && !connected);
    mux_pending = true;
    assert(!hub_classic_output_connect()); /* Old singleton channels must retire first. */
    mux_pending = false;
    assert(!hub_classic_output_set_target(stranger));
    ++mock_handle;
    ++mock_cid;
    assert(hub_classic_output_accept_connection(pc));
    connection_complete();
    emit_hid(HID_SUBEVENT_CONNECTION_CLOSED, 0); /* Old helper's cid0 close during a new ACL. */
    assert(handle == mock_handle);
    hid_open();
    drain();
    assert(hub_output_ready(0) && writes == 1);
    disconnected(previous_handle);
    emit_hid(HID_SUBEVENT_SUSPEND, previous_cid);
    emit_hid(HID_SUBEVENT_CONNECTION_CLOSED, 0);
    assert(hub_output_ready(0));
    uint8_t stale[] = {GAP_EVENT_SECURITY_LEVEL, 4, 0, 0, LEVEL_0, 1};
    little_endian_store_16(stale, 2, previous_handle);
    hci_event(HCI_EVENT_PACKET, 0, stale, sizeof(stale));
    assert(hub_output_ready(0));
    assert(hub_output_active_handle() == mock_handle);
    encryption_size = 0;
    service(&service_timer);
    assert(disconnecting && !hub_output_ready(0));
    assert(hub_output_active_handle() == HCI_CON_HANDLE_INVALID);
}

static void test_queue_reports(void) {
    reset();
    ready();
    report_count = 0;
    assert(hub_output_keyboard(key_down));
    assert(hub_output_mouse(1, 30000, -30000, 0));
    assert(hub_output_mouse(1, 30000, -30000, 0));
    assert(count == 2 && merges == 1);
    assert(hub_output_mouse(1, 0, 0, 129));
    assert(hub_output_mouse(1, 0, 0, -129));
    assert(hub_output_mouse(0, 0, 0, 0));
    uint8_t neutral[8] = {0};
    assert(hub_output_keyboard(neutral));
    clock_us += 5000;
    drain();
    assert(report_count == 9);
    assert(reports[0].data[1] == 1 && reports[0].data[4] == 4);
    assert((int16_t)little_endian_read_16(reports[1].data, 3) == 32767);
    assert((int16_t)little_endian_read_16(reports[2].data, 3) == 27233);
    assert((int16_t)little_endian_read_16(reports[1].data, 5) == -32768);
    assert((int16_t)little_endian_read_16(reports[2].data, 5) == -27232);
    assert((int8_t)reports[3].data[7] == 127 && (int8_t)reports[4].data[7] == 2);
    assert((int8_t)reports[5].data[7] == -128 && (int8_t)reports[6].data[7] == -1);
    assert(reports[7].data[2] == 0 && reports[8].data[1] == 1);
    assert(metrics[0].submitted == 2 && metrics[1].submitted == 7);
    assert(perf_samples == 9 && metrics[0].bins[2]);

    assert(hub_output_keyboard(key_down));
    send_error = BTSTACK_ACL_BUFFERS_FULL;
    if (!pending_send) tick();
    send_credit();
    assert(count == 1 && metrics[0].errors == 1 && retries == 1);
    send_error = 0;
    drain();
    assert(last_keyboard[2] == 4 && !count);
}

static void test_control_and_releases(void) {
    reset();
    ready();
    hub_output_keyboard(key_down);
    hub_output_mouse(3, 42, 0, 0);
    drain();
    uint8_t report[8];
    int size;
    assert(get_report(mock_cid, HID_REPORT_TYPE_INPUT, 1, &size, report) == 1 && size == 8 && report[2] == 4);
    assert(get_report(mock_cid, HID_REPORT_TYPE_INPUT, 2, &size, report) == 1 && size == 6 && report[0] == 3);
    assert(report[1] == 0 && report[2] == 0);
    uint8_t led_report[] = {1, 0xff};
    set_report(mock_cid, HID_REPORT_TYPE_OUTPUT, sizeof(led_report), led_report);
    assert(leds == 0x1f);
    set_report(mock_cid, HID_REPORT_TYPE_INPUT, sizeof(led_report), led_report);
    assert(leds == 0x1f);
    assert(get_report(mock_cid, HID_REPORT_TYPE_FEATURE, 3, &size, report) == -1);
    emit_hid(HID_SUBEVENT_SUSPEND, mock_cid);
    assert(!hub_output_ready(0) && releases == RELEASE_BOTH && router_releases);
    assert(!hub_output_keyboard(key_down) && get_report(mock_cid, HID_REPORT_TYPE_INPUT, 1, &size, report) == 0);
    emit_hid(HID_SUBEVENT_EXIT_SUSPEND, mock_cid);
    assert(!hub_output_ready(0));
    drain();
    assert(hub_output_ready(0) && last_keyboard[2] == 0 && last_buttons == 0);
    hub_output_keyboard(key_down);
    hub_output_release_all();
    assert(!count && releases == RELEASE_BOTH);
    if (pending_send) send_credit();
    assert(releases == RELEASE_MOUSE);
    hub_output_release_all();
    assert(releases == RELEASE_MOUSE);
    drain();
    boot_mode = true;
    assert(!hub_output_ready(0) && !hub_output_mouse(0, 1, 0, 0));
    boot_mode = false;
    emit_hid(HID_SUBEVENT_VIRTUAL_CABLE_UNPLUG, mock_cid);
    assert(disconnecting && writes == 1 && !hub_output_ready(0));
}

static void test_bounds_and_metrics(void) {
    reset();
    ready();
    assert(!hub_output_mouse(0, INT_MIN, INT_MAX, INT_MIN));
    assert(overflows == 1 && !count && releases);
    drain();
    unsigned first = report_count;
    assert(hub_output_mouse(0, -32768 * 32, 32767 * 32, -128 * 32));
    send_credit();
    assert((int16_t)little_endian_read_16(reports[first].data, 3) == -32768);
    assert((int16_t)little_endian_read_16(reports[first].data, 5) == 32767);
    assert((int8_t)reports[first].data[7] == -128);
    drain();
    assert(report_count - first == 32);
    assert(!hub_output_mouse(0, 0, 0, INT_MAX));
    assert(overflows == 2 && !count && releases);
    drain();
    assert(hub_output_mouse(0, 32767 * 32, 0, 0));
    assert(!hub_output_mouse(0, 1, 0, 0));
    assert(overflows == 3 && !count && releases);
    drain();
    for (unsigned i = 0; i < QUEUE_LENGTH; ++i) assert(hub_output_keyboard(key_down));
    assert(!hub_output_keyboard(key_down));
    assert(overflows == 4 && !count && releases);
    drain();
    hub_output_mouse(0, 1, 0, 0);
    hub_output_latency_reset();
    assert(highwater == 1 && metrics[1].enqueued == 0);
    hub_output_mouse(0, 2, 0, 0);
    assert(!queue[head].timing_valid && metrics[1].enqueued == 1);
    drain();
    assert(metrics[1].submitted == 1 && !metrics[1].samples);
    hub_output_mouse(0, 1, 0, 0);
    drain();
    assert(metrics[1].samples == 1);
    hub_output_latency_status();
    hub_output_status();
}

static void test_reconnect_releases_and_sdp_failure(void) {
    reset();
    ready();
    hub_output_release_all();
    send_credit();
    assert(releases == RELEASE_MOUSE);
    channel_ready = false;
    disconnected(mock_handle);
    pending_send = false;
    assert(releases == RELEASE_BOTH);
    ++mock_handle;
    ++mock_cid;
    pairing_open = false;
    unsigned first = report_count;
    connection_complete();
    hid_open();
    drain();
    assert(report_count == first + 2);
    assert(reports[first].data[1] == 1 && reports[first + 1].data[1] == 2);
    assert(hub_output_ready(0) && writes == 1);

    reset();
    sdp_error = ERROR_CODE_MEMORY_CAPACITY_EXCEEDED;
    hub_output_init();
    hub_output_start();
    assert(!sdp_registered && !started && !store.loaded);
    assert(!hub_classic_output_set_target(pc) && !hub_output_ready(0));
}

static void test_verified_acl_has_no_flash_lookups(void) {
    reset();
    ready();
    pairing_open = false;
    hub_classic_output_pairing_changed(false);
    unsigned baseline = bond_reads;
    assert(baseline > 0 && bond_lookups == baseline);
    for (unsigned i = 0; i < 20; ++i) {
        assert(hub_classic_output_accept_connection(pc));
        assert(hub_output_ready(0) && hub_output_active_handle() == mock_handle);
        assert(hub_output_keyboard(key_down));
        assert(hub_output_mouse(0, 1, 2, 0));
        drain();
        service(&service_timer);
    }
    assert(bond_reads == baseline && bond_lookups == baseline);
    hub_output_latency_reset();
    assert(verified && !bond_lookups && !bond_lookup_sum_us && !bond_lookup_max_us);
    service(&service_timer);
    assert(bond_reads == baseline);
    channel_ready = false;
    disconnected(mock_handle);
    have_bond = false;
    assert(!verified && !hub_classic_output_accept_connection(pc));
    assert(bond_reads > baseline && bond_lookups > 0);
    have_bond = true;
    assert(hub_classic_output_accept_connection(pc));
    connection_complete();
    hid_open();
    drain();
    assert(hub_output_ready(0) && writes == 1);
    baseline = bond_reads;
    encryption_size = 0;
    service(&service_timer);
    assert(disconnecting && !verified && !hub_output_ready(0));
    assert(bond_reads == baseline);
}

static void test_pump_and_expiry(void) {
    reset();
    hub_classic_output_set_target(pc);
    pairing_open = true;
    hub_classic_output_allow_pairing(pc);
    clock_ms = AUTH_TIMEOUT_MS + 1;
    pairing_open = false;
    assert(!hub_classic_output_accept_connection(pc));
    service(&service_timer);
    assert(!lease);
    reset();
    ready();
    for (unsigned i = 0; i < 20; ++i) hub_output_keyboard(key_down);
    clock_ms += SEND_TIMEOUT_MS + 1;
    service(&service_timer);
    assert(retries == 1 && count == 20);
    pending_send = false;
    registered = false;
    sync_send = true;
    unsigned before = report_count;
    hub_output_set_low_latency(true);
    assert(report_count - before == 1 && count == 19 && max_depth == 1);
    tick();
    assert(count == 18);
    drain();
    assert(max_depth == 1);
    assert(metrics[0].submitted == 20);
    for (unsigned i = before + 1; i < report_count; ++i)
        assert(reports[i].at_us - reports[i - 1].at_us == SEND_PERIOD_US);

    reset();
    hub_classic_output_set_target(pc);
    pairing_open = true;
    hub_classic_output_accept_connection(pc);
    connection_complete();
    pairing_open = false;
    hub_classic_output_pairing_changed(false);
    assert(disconnecting && !lease && !writes);
    clock_ms = AUTH_TIMEOUT_MS + 1;
    assert(!hub_classic_output_accept_connection(pc));
    service(&service_timer);
    assert(disconnecting && !writes && disconnects);
}

static void test_microsecond_cadence_and_safe_release(void) {
    reset();
    ready();
    sync_send = true;
    hub_output_set_low_latency(true);
    unsigned first = report_count;
    clock_us = UINT32_MAX - 1000u;
    clock_ms = UINT32_MAX - 1u;
    assert(hub_output_keyboard(key_down));
    assert(report_count == first + 1);
    assert(hub_output_mouse(1, 1, 0, 0));
    assert(timer_pending && timer_due_us - clock_us == SEND_PERIOD_US);
    clock_us += SEND_PERIOD_US - 1u;
    ++clock_ms;
    pump();
    assert(report_count == first + 1);
    tick();
    assert(report_count == first + 2 && !count);
    assert(reports[first + 1].at_us - reports[first].at_us == SEND_PERIOD_US);
    assert(gap_samples == 1 && gap_min_us == SEND_PERIOD_US);
    assert(hub_output_mouse(0, 2, 0, 0));
    hub_output_latency_reset();
    assert(cadence_remaining() == SEND_PERIOD_US && !gap_samples);
    hub_output_release_all();
    assert(!count && !releases && report_count == first + 4);
    assert(reports[first + 2].data[1] == 1 && reports[first + 3].data[2] == 0);
    tick(); /* A stale scheduled wakeup cannot replay a discarded report. */
    assert(report_count == first + 4);
    clock_ms += 5000000u;
    clock_us += 5000000000ULL;
    assert(hub_output_keyboard(key_down)); /* No wrap-induced idle delay. */
    assert(report_count == first + 5);
    timer_error = true;
    assert(hub_output_mouse(0, 1, 0, 0));
    assert(disconnecting && !hub_output_ready(0) && !count);
}

static void test_pc_link_mode_telemetry(void) {
    reset();
    assert(link_mode == 0xff);
    ready();
    assert(link_mode == 0 && mode_interval == 0);
    uint8_t mode[] = {HCI_EVENT_MODE_CHANGE, 6, 0, 0, 0, 2, 160, 0};
    little_endian_store_16(mode, 3, mock_handle + 1);
    hci_event(HCI_EVENT_PACKET, 0, mode, sizeof(mode));
    assert(link_mode == 0);
    little_endian_store_16(mode, 3, mock_handle);
    hci_event(HCI_EVENT_PACKET, 0, mode, 7);
    assert(link_mode == 0);
    hci_event(HCI_EVENT_PACKET, 0, mode, sizeof(mode));
    assert(link_mode == 2 && mode_interval == 160 && hub_output_ready(0));
    mode[2] = ERROR_CODE_COMMAND_DISALLOWED;
    mode[5] = 0;
    hci_event(HCI_EVENT_PACKET, 0, mode, sizeof(mode));
    assert(link_mode == 2 && mode_status == ERROR_CODE_COMMAND_DISALLOWED);
    hub_output_latency_reset();
    assert(link_mode == 2 && mode_interval == 160);
    mode[2] = 0;
    mode[6] = 0;
    hci_event(HCI_EVENT_PACKET, 0, mode, sizeof(mode));
    assert(link_mode == 0 && mode_interval == 0 && !mode_status);
    disconnected(mock_handle);
    assert(link_mode == 0xff && mode_interval == 0);
}

static void test_paced_motion_merging(void) {
    reset();
    ready();
    sync_send = true;
    unsigned first = report_count;
    assert(hub_output_mouse(0, 1, 1, 0));
    assert(report_count == first + 1);
    clock_us += 1000u;
    ++clock_ms;
    assert(hub_output_mouse(0, 2, 3, 0));
    clock_us += 1000u;
    ++clock_ms;
    assert(hub_output_mouse(0, 4, 5, 0));
    assert(report_count == first + 1 && count == 1 && merges == 1);
    assert(timer_due_us == reports[first].at_us + SEND_PERIOD_US);
    tick();
    assert(report_count == first + 2 && !count);
    assert(little_endian_read_16(reports[first + 1].data, 3) == 6);
    assert(little_endian_read_16(reports[first + 1].data, 5) == 8);
    assert(reports[first + 1].at_us - reports[first].at_us == SEND_PERIOD_US);
    assert(hub_output_mouse(1, 0, 0, 0));
    assert(hub_output_mouse(0, 0, 0, 0));
    assert(hub_output_mouse(0, 0, 0, 1));
    assert(hub_output_mouse(0, 0, 0, -1));
    assert(count == 4 && merges == 1);
    drain();
    assert(report_count == first + 6);
    assert(reports[first + 2].data[2] == 1 && reports[first + 3].data[2] == 0);
    assert(reports[first + 4].data[7] == 1 && reports[first + 5].data[7] == 0xff);
    for (unsigned i = first + 2; i < report_count; ++i)
        assert(reports[i].at_us - reports[i - 1].at_us == SEND_PERIOD_US);
    tick();
    assert(report_count == first + 6 && !timer_pending);
}

static void test_send_availability_timing(void) {
    reset();
    ready();
    hub_output_latency_reset();
    free_acl_slots = 0;
    assert(hub_output_mouse(0, 1, 0, 0));
    assert(registered && send_timing_valid);
    clock_us += 120000u;
    clock_ms += 120u;
    free_acl_slots = 3;
    send_credit();
    assert(send_wait_samples == 1 && send_wait_sum_us == 120000u);
    assert(send_wait_max_us == 120000u && send_wait_slow == 1);
    assert(send_blockers[1].slow == 1 && send_blockers[1].max_us == 120000u);
    assert(!send_timing_valid && !registered && !count);
    clock_us += SEND_PERIOD_US;
    clock_ms += (SEND_PERIOD_US + 999u) / 1000u;
    assert(hub_output_mouse(0, 2, 0, 0));
    assert(registered && send_timing_valid);
    hub_output_latency_reset();
    send_credit();
    assert(!send_wait_samples && !count);
    assert(!send_blockers[1].samples);
    clock_us += SEND_PERIOD_US;
    clock_ms += (SEND_PERIOD_US + 999u) / 1000u;
    sync_send = true;
    assert(hub_output_mouse(0, 3, 0, 0));
    assert(send_wait_samples == 1 && !send_wait_max_us);
    assert(!send_wait_slow && !send_timing_valid);
    assert(send_blockers[0].samples == 1);
    packet_buffer_reserved = true;
    assert(send_blockers_now() == 2);
    transport_available = false;
    assert(send_blockers_now() == 6);
    free_acl_slots = 0;
    assert(send_blockers_now() == 3);
    packet_buffer_reserved = false;
    assert(send_blockers_now() == 1);
}

static void test_controller_buffer_capacity(void) {
    reset();
    uint8_t classic[] = {HCI_EVENT_COMMAND_COMPLETE, 11, 1, 0x05, 0x10, 0,
                         0xfd, 3, 0, 9, 0, 0, 0};
    hci_event(HCI_EVENT_PACKET, 0, classic, sizeof(classic) - 1);
    assert(!controller_buffers_seen);
    hci_event(HCI_EVENT_PACKET, 0, classic, sizeof(classic));
    assert(controller_buffers_seen && controller_buffers == 9 && controller_packet_length == 1021);
    uint8_t le[] = {HCI_EVENT_COMMAND_COMPLETE, 7, 1, 0x02, 0x20, 0, 0xfb, 0, 3};
    hci_event(HCI_EVENT_PACKET, 0, le, sizeof(le));
    assert(controller_le_buffers_seen && controller_le_buffers == 3 && controller_le_packet_length == 251);
    le[5] = ERROR_CODE_COMMAND_DISALLOWED;
    le[8] = 0;
    hci_event(HCI_EVENT_PACKET, 0, le, sizeof(le));
    assert(controller_le_buffers == 3);
    hub_output_latency_reset();
    assert(controller_buffers_seen && controller_buffers == 9 && controller_le_buffers == 3);
    le[5] = 0;
    hci_event(HCI_EVENT_PACKET, 0, le, sizeof(le));
    assert(controller_le_buffers_seen && controller_le_buffers == 0);
}

static void test_incoming_security_start_delay(void) {
    reset();
    assert(hub_classic_output_set_target(pc));
    pairing_open = true;
    assert(hub_classic_output_accept_connection(pc));
    connection_complete();
    assert(security_start_pending && !security_requests && !hub_output_ready(0));
    clock_ms = AUTH_START_DELAY_MS - 1;
    service(&service_timer);
    assert(!security_requests);
    clock_ms++;
    service(&service_timer);
    assert(security_requests == 1 && !security_start_pending && !hub_output_ready(0));
    service(&service_timer);
    assert(security_requests == 1);
    disconnected(mock_handle);
    connection_complete();
    encryption_size = 16;
    security_level = LEVEL_2;
    clock_ms += AUTH_START_DELAY_MS;
    service(&service_timer);
    assert(security_requests == 1 && !security_start_pending);
    assert(!hub_output_ready(0)); /* Encrypted ACL alone never authorizes reports. */
    disconnected(mock_handle);
    encryption_size = 0;
    security_level = LEVEL_0;
    connection_complete();
    assert(security_start_pending);
    disconnected(mock_handle);
    clock_ms += AUTH_START_DELAY_MS;
    service(&service_timer);
    assert(!security_start_pending && security_requests == 1);
}

int main(void) {
#if HUB_CLASSIC_OUTPUT_7P5_TEST
    assert(SEND_PERIOD_US == 7500u);
#else
    assert(SEND_PERIOD_US == 2500u);
#endif
    test_authorization();
    test_storage_security();
    test_storage_namespace();
    test_reconnect_stale();
    test_queue_reports();
    test_control_and_releases();
    test_bounds_and_metrics();
    test_reconnect_releases_and_sdp_failure();
    test_verified_acl_has_no_flash_lookups();
    test_pump_and_expiry();
    test_microsecond_cadence_and_safe_release();
    test_paced_motion_merging();
    test_pc_link_mode_telemetry();
    test_send_availability_timing();
    test_controller_buffer_capacity();
    test_incoming_security_start_delay();
    puts("Classic output tests passed");
    return 0;
}
