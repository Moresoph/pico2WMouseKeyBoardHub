#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>

#ifdef __TINYC__
#define INPUT_TEST_JOIN_(a, b) a##b
#define INPUT_TEST_JOIN(a, b) INPUT_TEST_JOIN_(a, b)
#define _Static_assert(condition, message) \
    typedef char INPUT_TEST_JOIN(input_static_assert_, __LINE__)[(condition) ? 1 : -1]
#endif

/* Exercise the production state machine, with only radio/flash/router boundaries mocked. */
#include "..\src\hub_input.c"

static uint32_t clock_ms;
static bool pc_pairing, fail_write, fail_delete;
static unsigned flash_writes;
static unsigned connects, disconnects, pairing_requests, scan_starts, inquiry_starts;
static unsigned incoming_accepts, incoming_declines;
static unsigned connect_cancels;
static uint8_t classic_accept_policy, role_request_status;
static unsigned role_requests, role_queries;
static hci_role_t classic_role;
static bd_addr_t role_request_address;
static uint8_t connected_type;
static bd_addr_t connected_address;
static uint8_t flash_records[2][HUB_INPUT_TARGET_BYTES + 1u];
static unsigned flash_lengths[2];
static struct {
    int type;
    bd_addr_t address;
    bool key;
    bool irk;
} le_db[8];
static bd_addr_t classic_keys[8];
static bool classic_key_present[8];
static int handle_index[8];
static unsigned released[2], routed_keyboard, routed_mouse;
static unsigned ready_notifications[2];
static unsigned mouse_activity_calls;
static uint16_t mouse_activity_handle;
static unsigned hogp_started[2], hogp_stopped[2], phases[HUB_PERF_INPUT_CONNECT_FAILED + 1];
static unsigned iterator_index;
static const uint8_t keyboard_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0xc0
};
static const uint8_t mouse_map[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x30,
    0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0
};

static int flash_get(void *context, uint32_t tag, uint8_t *data, uint32_t length) {
    (void)context;
    assert(tag >= INPUT_TARGET_TAG && tag < INPUT_TARGET_TAG + 2);
    unsigned i = tag - INPUT_TARGET_TAG;
    unsigned copy = flash_lengths[i] < length ? flash_lengths[i] : length;
    if (data) memcpy(data, flash_records[i], copy);
    return (int)flash_lengths[i];
}

static int flash_store(void *context, uint32_t tag, const uint8_t *data, uint32_t length) {
    (void)context;
    assert(tag >= INPUT_TARGET_TAG && tag < INPUT_TARGET_TAG + 2);
    assert(length <= sizeof(flash_records[0]));
    if (fail_write) return -1;
    ++flash_writes;
    unsigned i = tag - INPUT_TARGET_TAG;
    memcpy(flash_records[i], data, length);
    flash_lengths[i] = length;
    return 0;
}

static void flash_delete(void *context, uint32_t tag) {
    (void)context;
    assert(tag >= INPUT_TARGET_TAG && tag < INPUT_TARGET_TAG + 2);
    if (!fail_delete) flash_lengths[tag - INPUT_TARGET_TAG] = 0;
}

static const btstack_tlv_t mock_tlv = { flash_get, flash_store, flash_delete };
void btstack_tlv_get_instance(const btstack_tlv_t **impl, void **context) {
    *impl = &mock_tlv;
    *context = NULL;
}
uint32_t btstack_run_loop_get_time_ms(void) { return clock_ms; }
void btstack_run_loop_add_timer(btstack_timer_source_t *timer) { (void)timer; }
void btstack_run_loop_set_timer(btstack_timer_source_t *timer, uint32_t ms) { (void)timer; (void)ms; }
void btstack_run_loop_set_timer_handler(btstack_timer_source_t *timer, void (*handler)(btstack_timer_source_t *)) {
    timer->process = handler;
}
bool hub_security_input_pairing_blocked(void) { return pc_pairing; }
void hub_latency_input_report(unsigned input, bool mouse) { (void)input; (void)mouse; }
void hub_latency_mouse_activity(uint16_t handle) {
    ++mouse_activity_calls;
    mouse_activity_handle = handle;
}
void hub_hogp_init(void) {
    memset(hogp_started, 0, sizeof(hogp_started));
    memset(hogp_stopped, 0, sizeof(hogp_stopped));
    memset(phases, 0, sizeof(phases));
}
void hub_hogp_start(unsigned input, uint16_t handle) { (void)handle; ++hogp_started[input]; }
void hub_hogp_stop(unsigned input) { ++hogp_stopped[input]; }
void hub_hogp_tick(void) {}
void hub_hogp_status(unsigned input) { (void)input; }
void hub_perf_input_phase(unsigned input, uint16_t handle, hub_perf_input_phase_t phase, uint8_t status) {
    (void)handle; (void)status; assert(input < 2); ++phases[phase];
}
void gap_set_connection_parameters(uint16_t interval, uint16_t window, uint16_t min_interval,
                                  uint16_t max_interval, uint16_t latency, uint16_t timeout,
                                  uint16_t min_ce, uint16_t max_ce) {
    assert(interval == 0x60 && window == 0x30 && min_interval == 8 && max_interval == 24);
    assert(latency == 4 && timeout == 72 && min_ce == 0 && max_ce == 0);
}
void hub_router_input_disconnected(unsigned i) { ++released[i]; }
void hub_router_input_connected(unsigned i) { ++ready_notifications[i]; }
void hub_router_keyboard(unsigned i, const uint8_t report[8]) { (void)i; (void)report; ++routed_keyboard; }
void hub_router_mouse(unsigned i, uint8_t b, int32_t x, int32_t y, int32_t w) {
    (void)i; (void)b; (void)x; (void)y; (void)w; ++routed_mouse;
}
void hci_add_event_handler(btstack_packet_callback_registration_t *registration) { (void)registration; }
void hci_set_master_slave_policy(uint8_t policy) { classic_accept_policy = policy; }
hci_role_t gap_get_role(hci_con_handle_t handle) {
    assert(handle != HCI_CON_HANDLE_INVALID);
    ++role_queries;
    return classic_role;
}
uint8_t gap_request_role(const bd_addr_t address, hci_role_t role) {
    assert(role == HCI_ROLE_MASTER);
    ++role_requests;
    memcpy(role_request_address, address, sizeof(bd_addr_t));
    return role_request_status;
}
void sm_add_event_handler(btstack_packet_callback_registration_t *registration) { (void)registration; }
void sm_bonding_decline(hci_con_handle_t handle) { (void)handle; }
void sm_request_pairing(hci_con_handle_t handle) { (void)handle; ++pairing_requests; }
int sm_le_device_index(hci_con_handle_t handle) { return handle < 8 ? handle_index[handle] : -1; }
int le_device_db_max_count(void) { return 8; }
int le_device_db_count(void) {
    int count = 0;
    for (unsigned i = 0; i < 8; ++i) if (le_db[i].type != BD_ADDR_TYPE_UNKNOWN) ++count;
    return count;
}
void le_device_db_info(int i, int *type, bd_addr_t address, sm_key_t irk) {
    assert(i >= 0 && i < 8);
    if (type) *type = le_db[i].type;
    if (address) memcpy(address, le_db[i].address, 6);
    if (irk) memset(irk, le_db[i].irk ? 0x67 : 0, 16);
}
void le_device_db_encryption_get(int i, uint16_t *ediv, uint8_t rand[8], sm_key_t key,
                                  int *size, int *authenticated, int *authorized, int *sc) {
    (void)ediv; (void)rand; (void)authenticated; (void)authorized; (void)sc;
    assert(i >= 0 && i < 8);
    if (key) memset(key, le_db[i].key ? 0x42 : 0, 16);
    if (size) *size = le_db[i].key ? 16 : 0;
}
void gap_delete_bonding(bd_addr_type_t type, bd_addr_t address) {
    /* Deliberately matches the SDK's exact LE type/address lookup. */
    int i = le_identity_index((uint8_t)type, address);
    if (i >= 0) { le_db[i].type = BD_ADDR_TYPE_UNKNOWN; le_db[i].key = false; }
}
bool gap_get_link_key_for_bd_addr(bd_addr_t address, link_key_t key, link_key_type_t *type) {
    (void)key; (void)type;
    for (unsigned i = 0; i < 8; ++i) {
        if (classic_key_present[i] && !memcmp(classic_keys[i], address, 6)) return true;
    }
    return false;
}
void gap_drop_link_key_for_bd_addr(bd_addr_t address) {
    for (unsigned i = 0; i < 8; ++i) {
        if (!memcmp(classic_keys[i], address, 6)) classic_key_present[i] = false;
    }
}
int gap_link_key_iterator_init(btstack_link_key_iterator_t *iterator) {
    (void)iterator; iterator_index = 0; return 1;
}
int gap_link_key_iterator_get_next(btstack_link_key_iterator_t *iterator, bd_addr_t address,
                                  link_key_t key, link_key_type_t *type) {
    (void)iterator; (void)key; (void)type;
    while (iterator_index < 8) {
        unsigned i = iterator_index++;
        if (!classic_key_present[i]) continue;
        memcpy(address, classic_keys[i], 6);
        return 1;
    }
    return 0;
}
void gap_link_key_iterator_done(btstack_link_key_iterator_t *iterator) { (void)iterator; }
uint8_t gap_encryption_key_size(hci_con_handle_t handle) { return handle < 8 ? 16 : 0; }
uint8_t gap_load_resolving_list_from_le_device_db(void) { return 0; }
uint8_t gap_connect(const bd_addr_t address, bd_addr_type_t type) {
    ++connects;
    connected_type = (uint8_t)type;
    memcpy(connected_address, address, 6);
    return 0;
}
uint8_t gap_connect_cancel(void) { ++connect_cancels; return 0; }
uint8_t gap_disconnect(hci_con_handle_t handle) { (void)handle; ++disconnects; return 0; }
void gap_set_scan_parameters(uint8_t type, uint16_t interval, uint16_t window) {
    assert(type == 1); (void)interval; (void)window;
}
void gap_start_scan(void) { ++scan_starts; }
void gap_stop_scan(void) {}
int gap_inquiry_start(uint8_t duration) { (void)duration; ++inquiry_starts; return 0; }
int gap_inquiry_stop(void) { return 0; }
void hid_host_init(uint8_t *data, uint16_t size) { (void)data; (void)size; }
void hids_host_init(uint8_t *data, uint16_t size) { (void)data; (void)size; }
void hid_host_register_packet_handler(btstack_packet_handler_t handler) { (void)handler; }
uint8_t hid_host_connect(bd_addr_t address, hid_protocol_mode_t mode, uint16_t *cid) {
    (void)mode; *cid = 1; return gap_connect(address, BD_ADDR_TYPE_ACL);
}
uint8_t hid_host_accept_connection(uint16_t cid, hid_protocol_mode_t mode) {
    (void)cid; (void)mode; ++incoming_accepts; return 0;
}
uint8_t hid_host_decline_connection(uint16_t cid) { (void)cid; ++incoming_declines; return 0; }
const uint8_t *hid_descriptor_storage_get_descriptor_data(uint16_t cid) { (void)cid; return NULL; }
uint16_t hid_descriptor_storage_get_descriptor_len(uint16_t cid) { (void)cid; return 0; }
uint8_t hids_host_connect(hci_con_handle_t handle, btstack_packet_handler_t handler,
                         hid_protocol_mode_t mode, uint16_t *cid) {
    (void)handle; (void)handler; (void)mode; *cid = 2; return 0;
}
const uint8_t *hids_host_descriptor_storage_get_descriptor_data(uint16_t cid, uint8_t service) {
    (void)cid; (void)service; return NULL;
}
uint16_t hids_host_descriptor_storage_get_descriptor_len(uint16_t cid, uint8_t service) {
    (void)cid; (void)service; return 0;
}

void hci_dump_log(int level, const char *format, ...) { (void)level; (void)format; }
void btstack_assert_failed(const char *file, const uint16_t line_nr) {
    fprintf(stderr, "BTstack assertion: %s:%u\n", file, line_nr);
    abort();
}

static void reset_fixture(void) {
    memset(flash_lengths, 0, sizeof(flash_lengths));
    memset(le_db, 0, sizeof(le_db));
    memset(classic_key_present, 0, sizeof(classic_key_present));
    memset(corrupt_target, 0, sizeof(corrupt_target));
    memset(released, 0, sizeof(released));
    memset(ready_notifications, 0, sizeof(ready_notifications));
    mouse_activity_calls = 0;
    mouse_activity_handle = HCI_CON_HANDLE_INVALID;
    for (unsigned i = 0; i < 8; ++i) {
        le_db[i].type = BD_ADDR_TYPE_UNKNOWN;
        handle_index[i] = -1;
    }
    clock_ms = 100;
    pc_pairing = fail_write = fail_delete = false;
    flash_writes = 0;
    connects = disconnects = pairing_requests = scan_starts = inquiry_starts = 0;
    incoming_accepts = incoming_declines = 0;
    connect_cancels = 0;
    classic_accept_policy = 0xff;
    role_request_status = 0;
    role_requests = role_queries = 0;
    classic_role = HCI_ROLE_MASTER;
    memset(role_request_address, 0, sizeof(role_request_address));
    input_started = enrollment_active = enrollment_pending = false;
    establishing = pending_candidate = -1;
    forgetting = candidate_count = 0;
    scanning = SCAN_OFF;
    input_tlv = NULL;
    hub_input_init();
}

static void seed_target(unsigned slot, bool classic, uint8_t type, uint8_t roles, uint8_t id) {
    hub_input_target_t target = { .classic = classic, .type = type, .roles = roles,
                                 .address = {0xc1, 2, 3, 4, 5, 0} };
    target.address[5] = id;
    hub_input_target_encode(&target, flash_records[slot]);
    flash_lengths[slot] = HUB_INPUT_TARGET_BYTES;
    if (classic) {
        memcpy(classic_keys[slot], target.address, 6);
        classic_key_present[slot] = true;
    } else {
        le_db[slot].type = type;
        memcpy(le_db[slot].address, target.address, 6);
        le_db[slot].key = le_db[slot].irk = true;
        handle_index[slot] = (int)slot;
    }
}

static void tick(uint32_t advance) { clock_ms += advance; input_tick(&input_timer); }
static void finish_disconnect(input_slot_t *slot) {
    slot->handle = HCI_CON_HANDLE_INVALID;
    slot->cid = 0;
    if (establishing == (int)slot_index(slot)) establishing = -1;
    tick(100);
}

static void test_records_and_policy(void) {
    hub_input_target_t in = { .classic = false, .type = 1, .roles = 3,
                             .address = {0xc1, 2, 3, 4, 5, 6} }, out;
    uint8_t data[HUB_INPUT_TARGET_BYTES];
    hub_input_target_encode(&in, data);
    assert(hub_input_target_decode(data, sizeof(data), &out));
    assert(out.roles == 3 && out.type == 1 && !memcmp(in.address, out.address, 6));
    assert(!out.insecure && !out.forwarding);
    for (unsigned i = 0; i < sizeof(data); ++i) {
        data[i] ^= 1;
        assert(!hub_input_target_decode(data, sizeof(data), &out));
        data[i] ^= 1;
    }
    assert(!hub_input_target_decode(data, sizeof(data) - 1, &out));
    in.address[0] = 0x41;
    hub_input_target_encode(&in, data);
    assert(!hub_input_target_decode(data, sizeof(data), &out)); /* RPA */
    in.address[0] = 0x01;
    assert(!hub_input_stable_address(false, 1, in.address)); /* NRPA */
    assert(hub_input_stable_address(false, 0, in.address));
    assert(!hub_input_stable_address(true, 1, in.address));
    assert(hub_input_assign_roles(1, 1) == 0);
    assert(hub_input_assign_roles(3, 1) == 2);
    assert(hub_input_assign_roles(3, 0) == 3);
    assert(hub_input_classic_roles(0x0540) == 1);
    assert(hub_input_classic_roles(0x0580) == 2);
    assert(hub_input_classic_roles(0x05c0) == 3);
    assert(!hub_input_classic_roles(0x0440));
    const uint8_t hid[] = {3, 3, 0x12, 0x18};
    const uint8_t keyboard[] = {3, 0x19, 0xc1, 3};
    const uint8_t name[] = {6, 9, 'm', 'o', 'u', 's', 'e'};
    const uint8_t broken[] = {8, 3, 0x12, 0x18};
    assert(hub_input_ble_hid_advertisement(hid, sizeof(hid)));
    assert(hub_input_ble_hid_advertisement(keyboard, sizeof(keyboard)));
    assert(!hub_input_ble_hid_advertisement(name, sizeof(name)));
    assert(!hub_input_ble_hid_advertisement(broken, sizeof(broken)));
    assert(hub_input_enrollment_permitted(true, UINT32_MAX - 4, 10, false));
    assert(!hub_input_enrollment_permitted(true, 10, 10, false));
    assert(!hub_input_enrollment_permitted(true, 1, 10, true));

    hub_input_target_t open = {
        .classic = true, .insecure = true, .forwarding = true, .type = 0, .roles = 1,
        .address = {0, 0x18, 0, 1, 2, 3}
    };
    hub_input_target_encode(&open, data);
    assert(data[2] == 2 && data[12] == 3);
    assert(hub_input_target_decode(data, sizeof(data), &out) && out.insecure && out.forwarding);
    open.forwarding = false;
    hub_input_target_encode(&open, data);
    assert(hub_input_target_decode(data, sizeof(data), &out) && out.insecure && !out.forwarding);
    open.classic = false;
    hub_input_target_encode(&open, data);
    assert(!hub_input_target_decode(data, sizeof(data), &out));
}

static void test_empty_boot_and_window(void) {
    reset_fixture();
    hub_input_start();
    tick(60000);
    assert(!connects && !scan_starts && !inquiry_starts && !hub_input_enrolling());
    assert(hub_input_enroll_start());
    uint32_t deadline = enrollment_deadline;
    assert(!hub_input_enroll_start() && deadline == enrollment_deadline);
    tick(100);
    assert(scanning == SCAN_LE && scan_starts == 1);
    tick(INPUT_ENROLL_SCAN_MS + 250);
    assert(scanning == SCAN_CLASSIC && inquiry_starts == 1);
    clock_ms = deadline;
    tick(0);
    assert(!hub_input_enrolling() && scanning == SCAN_CLASSIC_STOPPING);
    pc_pairing = true;
    assert(!hub_input_enroll_start());
}

static void test_boot_privacy_and_backoff(void) {
    reset_fixture();
    seed_target(0, false, 1, 1, 1);
    seed_target(1, true, 0, 2, 2);
    hub_input_start();
    pc_pairing = true;
    tick(1000);
    assert(!connects);
    pc_pairing = false;
    tick(100);
    assert(connects == 1 && establishing == 0 && connected_type == BD_ADDR_TYPE_LE_RANDOM_IDENTITY);
    assert(!inputs[0].pairing_authorized && !pairing_requests);
    assert(!memcmp(connected_address, le_db[0].address, 6));
    close_input(&inputs[0], ERROR_CODE_CONNECTION_TIMEOUT, true);
    finish_disconnect(&inputs[0]);
    assert(inputs[0].state == INPUT_RETRY && connects == 2 && establishing == 1);
    close_input(&inputs[1], ERROR_CODE_CONNECTION_TIMEOUT, true);
    finish_disconnect(&inputs[1]);
    inputs[0].retries = INPUT_RETRIES;
    close_input(&inputs[0], ERROR_CODE_CONNECTION_TIMEOUT, true);
    finish_close(&inputs[0], clock_ms);
    assert(inputs[0].deadline == clock_ms + INPUT_RETRY_MAX_MS && inputs[0].stored);
    le_db[0].key = false;
    assert(!start_connection(&inputs[0]) && inputs[0].auth_failed && !inputs[0].pairing_authorized);
    assert(flash_lengths[0] && flash_lengths[1] && classic_key_present[1]);
}

static void test_forget_isolation_and_defer(void) {
    reset_fixture();
    seed_target(0, false, 1, 1, 1);
    seed_target(1, true, 0, 2, 2);
    for (unsigned i = 2; i < 6; ++i) {
        le_db[i].type = 0; le_db[i].address[0] = (uint8_t)i; le_db[i].key = true;
        classic_keys[i][0] = (uint8_t)i; classic_key_present[i] = true;
    }
    hub_input_start();
    inputs[0].handle = 0; inputs[0].state = INPUT_READY;
    inputs[1].handle = 1; inputs[1].state = INPUT_READY;
    pc_pairing = true;
    assert(!hub_input_forget(0) && !hub_input_forget(4));
    assert(hub_input_forget(1));
    assert(hub_input_enrolling() && inputs[0].state == INPUT_CLOSING && !inputs[0].pairing_authorized);
    assert(!hub_input_forget(2));
    tick(100);
    assert(disconnects == 1 && flash_lengths[0] && le_db[0].key);
    finish_disconnect(&inputs[0]);
    assert(!flash_lengths[0] && !le_db[0].key && inputs[1].state == INPUT_READY);
    assert(flash_lengths[1] && classic_key_present[1] && !released[1]);
    assert(enrollment_pending && !enrollment_active && !scan_starts);
    pc_pairing = false;
    tick(100);
    assert(enrollment_active && scanning == SCAN_LE);
    hub_input_enroll_stop();
    assert(hub_input_forget(2));
    finish_disconnect(&inputs[1]);
    assert(!classic_key_present[1] && !flash_lengths[1]);
    for (unsigned i = 2; i < 6; ++i) assert(le_db[i].key && classic_key_present[i]);
}

static void test_candidates_and_roles(void) {
    reset_fixture();
    hub_input_start();
    assert(hub_input_enroll_start());
    tick(100);
    const bd_addr_t candidate = {0xc1, 2, 3, 4, 5, 7};
    offer_candidate(true, 0, candidate);
    tick(100);
    assert(inputs[0].candidate && establishing == 0 && hub_input_is_expected_classic(candidate));
    inputs[0].handle = 0;
    inputs[0].state = INPUT_DISCOVERING;
    inputs[0].security_complete = true;
    assert(save_descriptor(&inputs[0], 0, keyboard_map, sizeof(keyboard_map)));
    memcpy(classic_keys[0], candidate, 6); classic_key_present[0] = true;
    input_ready(&inputs[0]);
    assert(inputs[0].stored && inputs[0].roles == 1 && enrollment_active);
    assert(!hub_input_is_expected_classic(candidate));
    input_slot_t *duplicate = &inputs[1];
    duplicate->selected = duplicate->candidate = duplicate->classic = true;
    duplicate->handle = 1; duplicate->state = INPUT_DISCOVERING;
    duplicate->security_complete = true;
    assert(save_descriptor(duplicate, 0, keyboard_map, sizeof(keyboard_map)));
    input_ready(duplicate);
    assert(duplicate->state == INPUT_CLOSING && duplicate->remove && !duplicate->stored);
    finish_disconnect(duplicate);
    assert(!inputs[1].selected && classic_key_present[0]);
    duplicate->selected = duplicate->candidate = duplicate->classic = true;
    duplicate->handle = 1; duplicate->state = INPUT_DISCOVERING;
    duplicate->security_complete = true;
    memcpy(duplicate->address, candidate, 6);
    duplicate->address[5] = 8;
    memcpy(classic_keys[1], duplicate->address, 6);
    classic_key_present[1] = true;
    assert(save_descriptor(duplicate, 0, mouse_map, sizeof(mouse_map)));
    input_ready(duplicate);
    assert(inputs[0].state == INPUT_READY && inputs[0].stored && duplicate->stored);
    assert(duplicate->roles == 2 && !enrollment_active);
}

static void test_identity_save_and_failures(void) {
    reset_fixture();
    hub_input_start();
    input_slot_t *slot = &inputs[0];
    slot->selected = true; slot->handle = 0; slot->state = INPUT_DISCOVERING;
    slot->report_count = 1; slot->reports[0].keyboard = true;
    slot->address_type = 1; slot->address[0] = 0x41;
    handle_index[0] = 0; le_db[0].type = 1;
    memcpy(le_db[0].address, (uint8_t[]){0xc1, 2, 3, 4, 5, 9}, 6);
    le_db[0].key = true;
    input_ready(slot);
    assert(!slot->stored && slot->state == INPUT_DISCOVERING); /* Await key distribution. */
    slot->security_complete = true;
    input_ready(slot);
    assert(slot->stored && !memcmp(slot->identity, le_db[0].address, 6));
    hub_input_target_t decoded;
    assert(hub_input_target_decode(flash_records[0], flash_lengths[0], &decoded));
    assert(decoded.address[0] == 0xc1 && slot->address[0] == 0x41);
    hub_input_disconnect(0);
    finish_disconnect(slot);
    assert(slot->paused && slot->stored && flash_lengths[0] && le_db[0].key);
    fail_delete = true;
    assert(hub_input_forget(1));
    tick(100);
    assert(!enrollment_pending && flash_lengths[0] && le_db[0].key && slot->state == INPUT_FAILED);

    reset_fixture();
    hub_input_start();
    slot = &inputs[0];
    slot->selected = slot->classic = slot->security_complete = true;
    slot->state = INPUT_DISCOVERING; slot->handle = 0;
    slot->report_count = 1; slot->reports[0].mouse = true;
    memcpy(slot->address, (uint8_t[]){1, 2, 3, 4, 5, 6}, 6);
    memcpy(classic_keys[0], slot->address, 6); classic_key_present[0] = true;
    fail_write = true;
    input_ready(slot);
    assert(!slot->stored && slot->storage_error && slot->state == INPUT_CLOSING);
    assert(classic_key_present[0] && !flash_lengths[0]);
}

static void test_inquiry_cancel_and_timeout(void) {
    reset_fixture();
    hub_input_start();
    assert(hub_input_enroll_start());
    enrollment_next_classic = true;
    tick(100);
    const bd_addr_t address = {1, 2, 3, 4, 5, 6};
    offer_candidate(true, 0, address);
    assert(scanning == SCAN_CLASSIC_STOPPING && !connects);
    tick(100);
    assert(!connects); /* Not safe to connect until the inquiry's completion. */
    uint8_t command_status[6] = {HCI_EVENT_COMMAND_STATUS, 4, 0, 1, 0, 0};
    little_endian_store_16(command_status, 4, HCI_OPCODE_HCI_INQUIRY);
    input_hci_event(HCI_EVENT_PACKET, 0, command_status, sizeof(command_status));
    assert(classic_inquiry_active && !connects);
    uint8_t inquiry_done[3] = {GAP_EVENT_INQUIRY_COMPLETE, 1, 0};
    input_hci_event(HCI_EVENT_PACKET, 0, inquiry_done, sizeof(inquiry_done));
    tick(100);
    assert(connects == 1 && inputs[0].candidate && hub_input_is_expected_classic(address));
    inputs[0].handle = 0;
    clock_ms = enrollment_deadline;
    assert(!hub_input_is_expected_classic(address)); /* Even before the timer fires. */
    tick(0);
    assert(!enrollment_active && inputs[0].state == INPUT_CLOSING && inputs[0].remove && disconnects);
    finish_disconnect(&inputs[0]);
    assert(!inputs[0].selected);
}

static void emit_le_connection(uint8_t role, uint8_t type, const uint8_t *address, const uint8_t *rpa) {
    uint8_t event[36] = {HCI_EVENT_META_GAP, 34, GAP_SUBEVENT_LE_CONNECTION_COMPLETE, 0};
    little_endian_store_16(event, 4, 0);
    event[6] = role;
    event[7] = type;
    reverse_bytes(address, event + 8, 6);
    if (rpa) reverse_bytes(rpa, event + 20, 6);
    input_hci_event(HCI_EVENT_PACKET, 0, event, sizeof(event));
}

static void test_connection_event_privacy_and_isolation(void) {
    reset_fixture();
    seed_target(0, false, 0, 1, 1);
    hub_input_start();
    tick(1000);
    assert(connected_type == BD_ADDR_TYPE_LE_PUBLIC_IDENTITY);
    const bd_addr_t rpa = {0x41, 2, 3, 4, 5, 6};
    emit_le_connection(HCI_ROLE_SLAVE, 2, inputs[0].identity, rpa);
    assert(inputs[0].handle == HCI_CON_HANDLE_INVALID && inputs[0].state == INPUT_CONNECTING);
    emit_le_connection(HCI_ROLE_MASTER, 2, inputs[0].identity, rpa);
    assert(inputs[0].handle == 0 && inputs[0].state == INPUT_DISCOVERING);

    reset_fixture();
    hub_input_start();
    assert(hub_input_enroll_start());
    tick(100);
    offer_candidate(false, 1, rpa);
    tick(100);
    le_db[0].type = 1;
    memcpy(le_db[0].address, (uint8_t[]){0xc1, 2, 3, 4, 5, 9}, 6);
    le_db[0].key = true; handle_index[0] = 0;
    emit_le_connection(HCI_ROLE_MASTER, 3, le_db[0].address, rpa);
    assert(inputs[0].handle == 0); /* Enhanced event's identity is not the selected RPA. */
    uint8_t identity[20] = {SM_EVENT_IDENTITY_CREATED, 18};
    little_endian_store_16(identity, 2, 0);
    little_endian_store_16(identity, 18, 0);
    input_sm_event(HCI_EVENT_PACKET, 0, identity, sizeof(identity));
    assert(inputs[0].identity_valid && inputs[0].identity[0] == 0xc1);
    assert(inputs[0].address[0] == 0x41 && !inputs[0].stored);
}

static void test_corruption_capacity_and_reset_both(void) {
    reset_fixture();
    seed_target(0, false, 1, 1, 1);
    flash_records[0][2] = 99;
    hub_input_start();
    tick(1000);
    assert(corrupt_target[0] && !connects && !hub_input_forget(1) && le_db[0].key);

    reset_fixture();
    hub_input_start();
    for (unsigned i = 0; i < 8; ++i) {
        le_db[i].type = 0; le_db[i].address[0] = (uint8_t)(i + 1); le_db[i].key = true;
    }
    assert(!hub_input_connect(0, false, 0, "AA:BB:CC:DD:EE:FF"));
    assert(!connects);
    for (unsigned i = 0; i < 8; ++i) assert(le_db[i].key);

    reset_fixture();
    seed_target(0, false, 0, 1, 1);
    seed_target(1, false, 1, 2, 2);
    hub_input_start();
    assert(hub_input_forget(3));
    tick(100);
    assert(!inputs[0].selected && !inputs[1].selected && enrollment_active);
    assert(!flash_lengths[0] && !flash_lengths[1] && !le_db[0].key && !le_db[1].key);
}

static void test_composite_role_choice(void) {
    reset_fixture();
    hub_input_start();
    assert(hub_input_enroll_start());
    input_slot_t *slot = &inputs[0];
    slot->selected = slot->candidate = slot->security_complete = true;
    slot->handle = 0;
    slot->state = INPUT_DISCOVERING;
    uint8_t composite[sizeof(keyboard_map) + sizeof(mouse_map)];
    memcpy(composite, keyboard_map, sizeof(keyboard_map));
    memcpy(composite + sizeof(keyboard_map), mouse_map, sizeof(mouse_map));
    assert(save_descriptor(slot, 0, composite, sizeof(composite)));
    handle_index[0] = 0; le_db[0].type = 1; le_db[0].key = true;
    memcpy(le_db[0].address, (uint8_t[]){0xc1, 2, 3, 4, 5, 6}, 6);
    input_ready(slot);
    assert(slot->stored && slot->roles == HUB_INPUT_ROLE_BOTH);
    assert(enrollment_active && !inputs[1].selected);
    input_slot_t *mouse = &inputs[1];
    mouse->selected = mouse->candidate = mouse->security_complete = true;
    mouse->handle = 1;
    mouse->state = INPUT_DISCOVERING;
    assert(save_descriptor(mouse, 0, mouse_map, sizeof(mouse_map)));
    handle_index[1] = 1; le_db[1].type = 1; le_db[1].key = true;
    memcpy(le_db[1].address, (uint8_t[]){0xc1, 2, 3, 4, 5, 7}, 6);
    input_ready(mouse);
    assert(mouse->stored && mouse->roles == HUB_INPUT_ROLE_MOUSE && !enrollment_active);
    reset_slot(&inputs[0]);
    reset_slot(&inputs[1]);
    load_targets();
    assert(inputs[0].stored && inputs[1].stored && !corrupt_target[1]);
}

static void test_only_connectable_discoverable_ble_candidates(void) {
    reset_fixture();
    hub_input_start();
    assert(hub_input_enroll_start());
    tick(100);
    const bd_addr_t address = {0xc1, 2, 3, 4, 5, 6};
    const uint8_t hid[] = {3, 3, 0x12, 0x18};
    const uint8_t flags[] = {2, 1, 6};
    const uint8_t full[] = {2, 1, 6, 3, 3, 0x12, 0x18};
    offer_ble_advertisement(4, 1, address, hid, sizeof(hid));
    offer_ble_advertisement(3, 1, address, full, sizeof(full));
    offer_ble_advertisement(2, 1, address, full, sizeof(full));
    offer_ble_advertisement(0, 1, address, hid, sizeof(hid));
    assert(pending_candidate < 0);
    offer_ble_advertisement(0, 1, address, flags, sizeof(flags));
    offer_ble_advertisement(4, 1, address, hid, sizeof(hid));
    assert(pending_candidate >= 0 && candidate_count == 1);
    hub_input_enroll_stop();
    assert(hub_input_enroll_start());
    offer_ble_advertisement(0, 1, address, flags, sizeof(flags));
    clock_ms += 2001;
    offer_ble_advertisement(4, 1, address, hid, sizeof(hid));
    assert(pending_candidate < 0);
}

static void test_ready_notification_once(void) {
    for (unsigned transport = 0; transport < 2; ++transport) {
        reset_fixture();
        seed_target(0, transport != 0, transport ? 0 : 1, 1, 1);
        hub_input_start();
        tick(1000);
        input_slot_t *slot = &inputs[0];
        slot->handle = 0;
        slot->state = INPUT_DISCOVERING;
        assert(save_descriptor(slot, 0, keyboard_map, sizeof(keyboard_map)));
        input_ready(slot);
        assert(!ready_notifications[0]);
        assert(!hogp_started[0]);
        slot->security_complete = true;
        input_ready(slot);
        assert(ready_notifications[0] == 1);
        input_ready(slot);
        tick(100);
        assert(ready_notifications[0] == 1 && !ready_notifications[1]);
        assert(hogp_started[0] == (transport ? 0u : 1u));
        assert(phases[HUB_PERF_INPUT_READY] == (transport ? 0u : 1u));

        close_input(slot, ERROR_CODE_CONNECTION_TIMEOUT, true);
        finish_disconnect(slot);
        clock_ms = slot->deadline;
        tick(0);
        assert(slot->state == INPUT_CONNECTING);
        slot->handle = 0;
        slot->state = INPUT_DISCOVERING;
        slot->security_complete = true;
        assert(save_descriptor(slot, 0, keyboard_map, sizeof(keyboard_map)));
        input_ready(slot);
        input_ready(slot);
        assert(ready_notifications[0] == 2 && !ready_notifications[1]);
        assert(hogp_started[0] == (transport ? 0u : 2u));
        assert(hogp_stopped[0] == (transport ? 0u : 1u));
    }
}

static void test_reconnect_intervals_and_cancellation(void) {
    assert(retry_delay(0) == 1000);
    assert(retry_delay(1) == 2000);
    assert(retry_delay(2) == 4000);
    for (unsigned retries = 3; retries <= 255; ++retries) {
        assert(retry_delay((uint8_t)retries) == 5000);
    }
    reset_fixture();
    seed_target(0, false, 1, 1, 1);
    hub_input_start();
    tick(1000);
    input_slot_t *slot = &inputs[0];
    assert(slot->deadline == clock_ms + INPUT_RECONNECT_ATTEMPT_MS);
    tick(INPUT_RECONNECT_ATTEMPT_MS - 1);
    assert(!connect_cancels && connects == 1);
    tick(1);
    assert(connect_cancels == 1 && slot->state == INPUT_CLOSING && establishing == 0);
    tick(5000);
    assert(connect_cancels == 1 && connects == 1); /* Await controller cancellation completion. */
    uint8_t cancelled[36] = {
        HCI_EVENT_META_GAP, 34, GAP_SUBEVENT_LE_CONNECTION_COMPLETE,
        ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER
    };
    input_hci_event(HCI_EVENT_PACKET, 0, cancelled, sizeof(cancelled));
    tick(0);
    assert(establishing == -1 && slot->state == INPUT_RETRY);
    uint32_t next = slot->deadline;
    clock_ms = next - 1u;
    tick(0);
    assert(connects == 1);
    tick(1);
    assert(connects == 2 && slot->state == INPUT_CONNECTING && !slot->pairing_authorized);

    close_input(slot, ERROR_CODE_CONNECTION_TIMEOUT, true);
    finish_disconnect(slot);
    slot->retries = 255;
    clock_ms = UINT32_MAX - 100u;
    close_input(slot, ERROR_CODE_CONNECTION_TIMEOUT, true);
    finish_close(slot, clock_ms);
    assert(slot->deadline == clock_ms + 5000u);
    tick(4999);
    assert(slot->state == INPUT_RETRY);
    tick(1);
    assert(slot->state == INPUT_CONNECTING);
}

static void test_transient_security_failure_retries_but_key_loss_stops(void) {
    const uint8_t transient[] = {
        ERROR_CODE_CONNECTION_TIMEOUT,
        ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION,
        ERROR_CODE_CONNECTION_TERMINATED_BY_LOCAL_HOST
    };
    for (unsigned i = 0; i < sizeof(transient); ++i) {
        reset_fixture();
        seed_target(0, false, 1, 1, 1);
        seed_target(1, true, 0, 2, 2);
        hub_input_start();
        input_slot_t *slot = &inputs[0];
        slot->handle = 0;
        slot->state = INPUT_SECURITY;
        inputs[1].state = INPUT_READY;
        inputs[1].handle = 1;
        uint8_t event[12] = {SM_EVENT_REENCRYPTION_COMPLETE, 10};
        event[11] = transient[i];
        input_sm_event(HCI_EVENT_PACKET, 0, event, sizeof(event));
        assert(slot->state == INPUT_CLOSING && slot->retry_allowed && !slot->auth_failed);
        finish_disconnect(slot);
        assert(slot->state == INPUT_RETRY && (uint32_t)(slot->deadline - clock_ms) <= 5000u);
        assert(inputs[1].state == INPUT_READY && !released[1]);
        assert(le_db[0].key && classic_key_present[1]);
    }
    reset_fixture();
    seed_target(0, false, 1, 1, 1);
    hub_input_start();
    inputs[0].handle = 0;
    inputs[0].state = INPUT_SECURITY;
    uint8_t event[12] = {SM_EVENT_REENCRYPTION_COMPLETE, 10};
    event[11] = ERROR_CODE_PIN_OR_KEY_MISSING;
    input_sm_event(HCI_EVENT_PACKET, 0, event, sizeof(event));
    assert(inputs[0].auth_failed && !inputs[0].retry_allowed);
    event[11] = ERROR_CODE_REMOTE_USER_TERMINATED_CONNECTION;
    input_sm_event(HCI_EVENT_PACKET, 0, event, sizeof(event));
    assert(inputs[0].auth_failed && !inputs[0].retry_allowed);
    finish_disconnect(&inputs[0]);
    assert(inputs[0].state == INPUT_FAILED && le_db[0].key && flash_lengths[0]);
}

static void test_manual_pause_does_not_auto_resume(void) {
    reset_fixture();
    seed_target(0, false, 1, 1, 1);
    hub_input_start();
    hub_input_disconnect(0);
    tick(100);
    tick(60000);
    assert(inputs[0].paused && inputs[0].state == INPUT_FAILED && connects == 0);
    assert(flash_lengths[0] && le_db[0].key);
}

static void test_secure_image_does_not_activate_saved_open_input(void) {
    reset_fixture();
    hub_input_target_t open = {
        .classic = true, .insecure = true, .forwarding = true, .type = 0, .roles = 1,
        .address = {0, 0x18, 0, 1, 2, 3}
    };
    hub_input_target_encode(&open, flash_records[1]);
    flash_lengths[1] = HUB_INPUT_TARGET_BYTES;
    hub_input_start();
    tick(60000);
    assert(!hub_input_open_bridge_enabled() && !connects);
    assert(inputs[1].stored && inputs[1].paused && inputs[1].state == INPUT_FAILED);
    assert(flash_lengths[1] == HUB_INPUT_TARGET_BYTES);
}

static void test_radio_busy_covers_establishment_and_scans(void) {
    reset_fixture();
    assert(!hub_input_radio_busy());
    scanning = SCAN_LE;
    assert(hub_input_radio_busy());
    scanning = SCAN_CLASSIC_STOPPING;
    assert(hub_input_radio_busy());
    scanning = SCAN_OFF;
    const input_state_t busy_states[] = {INPUT_CONNECTING, INPUT_SECURITY, INPUT_DISCOVERING, INPUT_CLOSING};
    inputs[0].selected = true;
    for (unsigned i = 0; i < sizeof(busy_states) / sizeof(busy_states[0]); ++i) {
        inputs[0].state = busy_states[i];
        assert(hub_input_radio_busy());
    }
    inputs[0].state = INPUT_READY;
    assert(!hub_input_radio_busy());
    inputs[0].state = INPUT_RETRY;
    assert(!hub_input_radio_busy());
    establishing = 0;
    assert(hub_input_radio_busy());
    establishing = -1;
    enrollment_pending = true;
    assert(hub_input_radio_busy());
}

static void test_classic_target_isolation_survives_pause(void) {
    reset_fixture();
    seed_target(0, true, 0, 1, 1);
    seed_target(1, false, 1, 2, 2);
    hub_input_start();
    assert(hub_input_is_classic_target(inputs[0].address));
    assert(!hub_input_is_classic_target(inputs[1].address));
    assert(!hub_input_is_classic_target(NULL));
    const uint8_t unknown[] = {1, 2, 3, 4, 5, 6};
    assert(!hub_input_is_classic_target(unknown));
    inputs[0].paused = true;
    inputs[0].state = INPUT_FAILED;
    assert(hub_input_is_classic_target(inputs[0].address));
    assert(!hub_input_is_expected_classic(inputs[0].address));
    inputs[0].remove = true;
    inputs[0].state = INPUT_CLOSING;
    assert(hub_input_is_classic_target(inputs[0].address));
    inputs[0].selected = false;
    assert(!hub_input_is_classic_target(inputs[0].address));
}

static void test_hids_flow_metadata(void) {
    reset_fixture();
    input_slot_t *slot = &inputs[0];
    slot->selected = true;
    slot->state = INPUT_READY;
    slot->cid = 1;
    slot->handle = 0x41;
    slot->service_count = 1;
    slot->roles = HUB_INPUT_ROLE_MOUSE;
    assert(save_descriptor(slot, 0, mouse_map, sizeof(mouse_map)));
    uint8_t packet[] = {HCI_EVENT_GATTSERVICE_META, 9, GATTSERVICE_SUBEVENT_HID_REPORT,
                         1, 0, 0, 0, 2, 0, 0, 5};
    uint8_t original[sizeof(packet)];
    memcpy(original, packet, sizeof(packet));
    clock_ms = 42;
    input_hids_event(HCI_EVENT_GATTSERVICE_META, 0, packet, sizeof(packet));
    assert(slot->flow.hids == 1 && slot->flow.parsed == 1 && slot->flow.mouse == 1);
    assert(slot->flow.hids_at == 42 && !slot->flow.invalid && routed_mouse == 1);
    assert(phases[HUB_PERF_INPUT_FIRST_HID] == 1 && phases[HUB_PERF_INPUT_FIRST_ROUTED] == 1);
    assert(!memcmp(original, packet, sizeof(packet)));
    assert(mouse_activity_calls == 1 && mouse_activity_handle == 0x41);
    packet[7] = 3; /* Truncated callback payload. */
    input_hids_event(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
    assert(slot->flow.rejected == 1 && slot->flow.parsed == 1);
    packet[7] = 1; /* Synthetic ID but missing required mouse payload. */
    input_hids_event(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
    assert(slot->flow.parsed == 2 && slot->flow.invalid == 1);
    slot->state = INPUT_DISCOVERING;
    input_hids_event(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
    assert(slot->flow.hids == 4 && slot->flow.rejected == 2);
    uint32_t before = unmatched_hids_reports;
    packet[3] = 2;
    input_hids_event(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
    assert(unmatched_hids_reports == before + 1 && slot->flow.hids == 4);
    assert(phases[HUB_PERF_INPUT_FIRST_HID] == 1 && phases[HUB_PERF_INPUT_FIRST_ROUTED] == 1);
    assert(mouse_activity_calls == 1);
    slot->state = INPUT_READY;
    slot->classic = true;
    const uint8_t motion[] = {5};
    handle_report(slot, 0, motion, sizeof(motion));
    assert(mouse_activity_calls == 1);
    slot->classic = false;
    memset(slot->reports, 0, sizeof(slot->reports));
    slot->report_count = 0;
    slot->roles = HUB_INPUT_ROLE_KEYBOARD;
    assert(save_descriptor(slot, 0, keyboard_map, sizeof(keyboard_map)));
    handle_report(slot, 0, motion, sizeof(motion));
    assert(mouse_activity_calls == 1);
}

static void test_hids_failure_never_starts_companion(void) {
    reset_fixture();
    input_slot_t *slot = &inputs[0];
    slot->selected = slot->stored = slot->security_complete = true;
    slot->state = INPUT_DISCOVERING;
    slot->handle = 0;
    slot->cid = 2;
    uint8_t packet[] = {HCI_EVENT_GATTSERVICE_META, 6, GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED,
                       2, 0, ERROR_CODE_UNSPECIFIED_ERROR, HID_PROTOCOL_MODE_REPORT, 1};
    input_hids_event(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
    assert(slot->state == INPUT_CLOSING && !slot->cid && slot->last_hid_status == ERROR_CODE_UNSPECIFIED_ERROR);
    assert(!hogp_started[0] && !ready_notifications[0] && !phases[HUB_PERF_INPUT_READY]);
}

static void test_ready_ble_mouse_identification(void) {
    reset_fixture();
    input_slot_t *slot = &inputs[0];
    slot->selected = true;
    slot->handle = 0x40;
    slot->roles = HUB_INPUT_ROLE_BOTH;
    slot->state = INPUT_DISCOVERING;
    assert(!hub_input_is_ble_mouse_ready(0x40));
    slot->state = INPUT_READY;
    assert(hub_input_is_ble_mouse_ready(0x40));
    assert(!hub_input_is_ble_mouse_ready(0x41));
    assert(!hub_input_is_ble_mouse_ready(HCI_CON_HANDLE_INVALID));
    slot->roles = HUB_INPUT_ROLE_KEYBOARD;
    assert(!hub_input_is_ble_mouse_ready(0x40));
    slot->roles = HUB_INPUT_ROLE_MOUSE;
    slot->classic = true;
    assert(!hub_input_is_ble_mouse_ready(0x40));
    slot->classic = false;
    slot->state = INPUT_CLOSING;
    assert(!hub_input_is_ble_mouse_ready(0x40));
}

static void classic_acl_event(input_slot_t *slot, uint16_t handle, uint8_t status, uint8_t type) {
    uint8_t packet[13] = {HCI_EVENT_CONNECTION_COMPLETE, 11};
    packet[2] = status;
    little_endian_store_16(packet, 3, handle);
    reverse_bd_addr(slot->address, &packet[5]);
    packet[11] = type;
    input_hci_event(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
}

static void test_classic_central_preference(void) {
    reset_fixture();
    assert(classic_accept_policy ==
           (HUB_CLASSIC_INPUT_CENTRAL && !HUB_CLASSIC_OUTPUT_TEST ? 0 : 1));
    input_slot_t *slot = &inputs[0];
    slot->selected = slot->classic = slot->stored = true;
    slot->address[0] = 0xc1;
    slot->address[5] = 1;
    slot->state = INPUT_CONNECTING;
    slot->deadline = clock_ms + INPUT_CONNECT_MS;

    classic_role = HCI_ROLE_SLAVE;
    classic_acl_event(slot, 0x40, ERROR_CODE_PAGE_TIMEOUT, 1);
    classic_acl_event(slot, 0x40, 0, 0); /* SCO is not an input ACL. */
    assert(!role_requests && !role_queries && slot->handle == HCI_CON_HANDLE_INVALID);
    slot->selected = false;
    classic_acl_event(slot, 0x40, 0, 1); /* Unknown/PC address: no policy request. */
    slot->selected = true;
    slot->classic = false;
    classic_acl_event(slot, 0x40, 0, 1); /* BLE target with same bytes is not Classic. */
    assert(!role_requests && !role_queries && slot->handle == HCI_CON_HANDLE_INVALID);
    slot->classic = true;
    classic_acl_event(slot, 0x40, 0, 1);
    assert(role_requests == (HUB_CLASSIC_INPUT_CENTRAL ? 1u : 0u));
    assert(slot->classic_central_requested == (bool)HUB_CLASSIC_INPUT_CENTRAL);
    assert(slot->state == INPUT_CONNECTING && slot->handle == 0x40);
    assert(!flash_writes && !disconnects && !slot->pairing_authorized);
    if (HUB_CLASSIC_INPUT_CENTRAL) {
        assert(!memcmp(role_request_address, slot->address, sizeof(bd_addr_t)));
    }
    classic_acl_event(slot, 0x40, 0, 1);
    unsigned requests = role_requests;
    assert(requests == (HUB_CLASSIC_INPUT_CENTRAL ? 1u : 0u));

    uint8_t changed[10] = {HCI_EVENT_ROLE_CHANGE, 8, 0};
    reverse_bd_addr(slot->address, &changed[3]);
    changed[9] = HCI_ROLE_MASTER;
    input_hci_event(HCI_EVENT_PACKET, 0, changed, sizeof(changed));
    changed[9] = HCI_ROLE_SLAVE;
    input_hci_event(HCI_EVENT_PACKET, 0, changed, sizeof(changed));
    changed[2] = ERROR_CODE_COMMAND_DISALLOWED;
    input_hci_event(HCI_EVENT_PACKET, 0, changed, sizeof(changed));
    input_hci_event(HCI_EVENT_PACKET, 0, changed, sizeof(changed) - 1);
    uint8_t command[6] = {HCI_EVENT_COMMAND_STATUS, 4, ERROR_CODE_COMMAND_DISALLOWED, 1};
    little_endian_store_16(command, 4, HCI_OPCODE_HCI_SWITCH_ROLE_COMMAND);
    input_hci_event(HCI_EVENT_PACKET, 0, command, sizeof(command));
    tick(1000);
    assert(role_requests == requests && slot->state == INPUT_CONNECTING);

    uint8_t disconnected[6] = {HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0};
    little_endian_store_16(disconnected, 3, 0x40);
    input_hci_event(HCI_EVENT_PACKET, 0, disconnected, sizeof(disconnected));
    assert(!slot->classic_central_requested && slot->handle == HCI_CON_HANDLE_INVALID);
    slot->state = INPUT_CONNECTING;
    role_request_status = ERROR_CODE_COMMAND_DISALLOWED;
    classic_acl_event(slot, 0x40, 0, 1); /* Reused handle is a new ACL lifetime. */
    assert(role_requests == requests + (HUB_CLASSIC_INPUT_CENTRAL ? 1u : 0u));
    assert(slot->state == INPUT_CONNECTING && !slot->auth_failed);
    requests = role_requests;
    classic_acl_event(slot, 0x40, 0, 1);
    assert(role_requests == requests); /* Even rejected submissions are not looped. */

    input_slot_t *other = &inputs[1];
    other->selected = other->classic = true;
    memcpy(other->address, slot->address, sizeof(bd_addr_t));
    other->address[5] = 2;
    other->state = INPUT_CONNECTING;
    classic_role = HCI_ROLE_MASTER;
    classic_acl_event(other, 0x41, 0, 1);
    assert(!other->classic_central_requested && role_requests == requests);
    classic_role = HCI_ROLE_INVALID;
    classic_acl_event(other, 0x41, 0, 1);
    assert(role_requests == requests);
    classic_role = HCI_ROLE_SLAVE;
    other->paused = true;
    classic_acl_event(other, 0x41, 0, 1);
    assert(other->state == INPUT_CLOSING && role_requests == requests);
    other->paused = false;
    other->state = INPUT_CLOSING;
    classic_acl_event(other, 0x41, 0, 1);
    assert(role_requests == requests);
    other->state = INPUT_CONNECTING;
    other->remove = true;
    classic_acl_event(other, 0x41, 0, 1);
    assert(role_requests == requests);
    other->remove = false;
    other->state = INPUT_CONNECTING;
    other->auth_failed = true;
    classic_acl_event(other, 0x41, 0, 1);
    assert(role_requests == requests);
    other->auth_failed = false;
    other->state = INPUT_CONNECTING;
    other->open_test = true;
    role_request_status = 0;
    classic_acl_event(other, 0x41, 0, 1);
    assert(role_requests == requests + (HUB_CLASSIC_INPUT_CENTRAL ? 1u : 0u));
    if (HUB_CLASSIC_INPUT_CENTRAL) {
        assert(!memcmp(role_request_address, other->address, sizeof(bd_addr_t)));
    }
    assert(!other->forwarding_authorized && !flash_writes);
}

int main(void) {
    test_classic_central_preference();
    test_hids_failure_never_starts_companion();
    test_ready_ble_mouse_identification();
    test_hids_flow_metadata();
    test_records_and_policy();
    test_empty_boot_and_window();
    test_boot_privacy_and_backoff();
    test_forget_isolation_and_defer();
    test_candidates_and_roles();
    test_identity_save_and_failures();
    test_inquiry_cancel_and_timeout();
    test_connection_event_privacy_and_isolation();
    test_corruption_capacity_and_reset_both();
    test_composite_role_choice();
    test_only_connectable_discoverable_ble_candidates();
    test_ready_notification_once();
    test_reconnect_intervals_and_cancellation();
    test_transient_security_failure_retries_but_key_loss_stops();
    test_manual_pause_does_not_auto_resume();
    test_secure_image_does_not_activate_saved_open_input();
    test_radio_busy_covers_establishment_and_scans();
    test_classic_target_isolation_survives_pause();
    puts("input tests: 22 groups passed (production state machine and SDK HID parser)");
    return 0;
}
