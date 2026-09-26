#include <assert.h>
#include <stdio.h>
#include "..\src\hub_latency.c"

static uint32_t milliseconds, microseconds;
static uint16_t active_handle, mouse_handle;
static uint16_t not_ready_handle;
static bool pairing_busy, input_busy, output_fast;
static uint16_t unencrypted_handle;
static unsigned resets, updates, requests;
static unsigned perf_reports, perf_resets, perf_requests, perf_timeouts;
static uint16_t changed_handle, minimum_requested, maximum_requested, latency_requested, timeout_requested;
static int request_error;
static uint8_t setting[9];
static int setting_length;
static bool fail_save;
static le_connection_parameter_range_t parameter_range;
static unsigned range_writes;

void gap_get_connection_parameter_range(le_connection_parameter_range_t *range) {
    *range = parameter_range;
}
void gap_set_connection_parameter_range(le_connection_parameter_range_t *range) {
    parameter_range = *range;
    ++range_writes;
}

static int read_setting(void *context, uint32_t tag, uint8_t *data, uint32_t size) {
    (void)context;
    assert(tag == LATENCY_SETTINGS_TAG);
    if (data) memcpy(data, setting, size < (uint32_t)setting_length ? size : (uint32_t)setting_length);
    return setting_length;
}
static int write_setting(void *context, uint32_t tag, const uint8_t *data, uint32_t size) {
    (void)context;
    assert(tag == LATENCY_SETTINGS_TAG && size == 8);
    if (fail_save) return -1;
    memcpy(setting, data, size);
    setting_length = (int)size;
    return 0;
}
static void delete_setting(void *context, uint32_t tag) {
    (void)context; (void)tag;
    assert(!"Latency mode must not delete other settings");
}
void btstack_tlv_get_instance(const btstack_tlv_t **tlv, void **context) {
    static const btstack_tlv_t implementation = {read_setting, write_setting, delete_setting};
    *tlv = &implementation;
    *context = NULL;
}

uint32_t hub_latency_now_us(void) { return microseconds; }
uint32_t btstack_run_loop_get_time_ms(void) { return milliseconds; }
void btstack_run_loop_set_timer(btstack_timer_source_t *source, uint32_t delay) {
    (void)source; (void)delay;
}
void btstack_run_loop_add_timer(btstack_timer_source_t *source) { (void)source; }
void btstack_run_loop_set_timer_handler(btstack_timer_source_t *source,
                                      void (*handler)(btstack_timer_source_t *)) { source->process = handler; }
void hci_add_event_handler(btstack_packet_callback_registration_t *handler) { (void)handler; }
void l2cap_add_event_handler(btstack_packet_callback_registration_t *handler) { (void)handler; }
bool hub_security_input_pairing_blocked(void) { return pairing_busy; }
bool hub_input_radio_busy(void) { return input_busy; }
bool hub_input_is_connection(uint16_t handle) { return handle == 1; }
bool hub_input_is_ble_mouse_ready(uint16_t handle) { return handle == mouse_handle; }
uint16_t hub_output_active_handle(void) { return active_handle; }
bool hub_output_is_ready_connection(uint16_t handle) {
    return handle >= 2 && handle <= 5 && handle != not_ready_handle;
}
void hub_output_set_low_latency(bool enabled) { output_fast = enabled; }
void hub_output_latency_reset(void) { ++resets; }
void hub_output_latency_status(void) {}
uint8_t gap_encryption_key_size(hci_con_handle_t handle) { return handle == unencrypted_handle ? 0 : 16; }
void hub_perf_input_report(unsigned input, bool mouse) { (void)input; (void)mouse; ++perf_reports; }
void hub_perf_reset(void) { ++perf_resets; }
void hub_perf_parameter_timeout(uint16_t handle) { (void)handle; ++perf_timeouts; }
void hub_perf_parameter_request(uint16_t handle, bool fast, uint16_t minimum, uint16_t maximum,
                               uint16_t latency, uint16_t timeout, int status) {
    (void)handle; (void)fast; (void)minimum; (void)maximum;
    (void)latency; (void)timeout; (void)status;
    ++perf_requests;
}
int gap_update_connection_parameters(hci_con_handle_t handle, uint16_t minimum,
                                     uint16_t maximum, uint16_t latency, uint16_t timeout) {
    ++updates; changed_handle = handle; minimum_requested = minimum;
    maximum_requested = maximum; latency_requested = latency; timeout_requested = timeout;
    return request_error;
}
int gap_request_connection_parameter_update(hci_con_handle_t handle, uint16_t minimum,
                                             uint16_t maximum, uint16_t latency, uint16_t timeout) {
    ++requests; changed_handle = handle; minimum_requested = minimum;
    maximum_requested = maximum; latency_requested = latency; timeout_requested = timeout;
    return request_error;
}

static void reset(void) {
    milliseconds = microseconds = 0;
    active_handle = HCI_CON_HANDLE_INVALID;
    mouse_handle = HCI_CON_HANDLE_INVALID;
    not_ready_handle = HCI_CON_HANDLE_INVALID;
    pairing_busy = input_busy = output_fast = false;
    unencrypted_handle = HCI_CON_HANDLE_INVALID;
    resets = updates = requests = 0;
    perf_reports = perf_resets = perf_requests = perf_timeouts = 0;
    request_error = 0;
    setting_length = 0;
    fail_save = false;
    hub_latency_init();
    parameter_range = (le_connection_parameter_range_t){6, 3200, 0, 500, 10, 3200};
    range_writes = 0;
    uint8_t state[] = {BTSTACK_EVENT_STATE, 1, HCI_STATE_WORKING};
    event_handler(HCI_EVENT_PACKET, 0, state, sizeof(state));
}

static void connect_link(uint16_t handle, uint8_t role) {
    uint8_t packet[36] = {HCI_EVENT_META_GAP, 34, GAP_SUBEVENT_LE_CONNECTION_COMPLETE, 0};
    little_endian_store_16(packet, 4, handle);
    packet[6] = role;
    little_endian_store_16(packet, 26, 24); /* 30 ms */
    little_endian_store_16(packet, 28, 4);
    little_endian_store_16(packet, 30, 200);
    event_handler(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
}

static void updated_parameters(uint16_t handle, uint8_t status, uint16_t interval,
                               uint16_t latency, uint16_t timeout) {
    uint8_t packet[12] = {HCI_EVENT_LE_META, 10, HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE};
    packet[3] = status;
    little_endian_store_16(packet, 4, handle);
    little_endian_store_16(packet, 6, interval);
    little_endian_store_16(packet, 8, latency);
    little_endian_store_16(packet, 10, timeout);
    event_handler(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
}

static void updated(uint16_t handle, uint8_t status, uint16_t interval) {
    updated_parameters(handle, status, interval, 0, 200);
}

static void tick(void) { milliseconds += 250; service(&timer); }
static void settle(void) {
    for (unsigned i = 0; i < 9; ++i) tick();
}

static void test_targeted_fast_and_restore(void) {
    reset();
    connect_link(1, HCI_ROLE_MASTER);
    connect_link(2, HCI_ROLE_SLAVE);
    connect_link(3, HCI_ROLE_SLAVE);
    active_handle = 2;
    milliseconds = 2000;
    settle();
    assert(!updates && !requests && !output_fast);
    assert(hub_latency_set_fast(true) && output_fast);
    tick();
    assert(updates == 1 && changed_handle == 1 && minimum_requested == 6 && maximum_requested == 12);
    assert(latency_requested == 0);
    updated(1, 0, 12);
    settle();
    assert(requests == 1 && changed_handle == 2);
    assert(minimum_requested == 6 && maximum_requested == 6 && latency_requested == 0);
    updated(2, 0, 6);
    for (unsigned i = 0; i < 8; ++i) tick();
    assert(updates == 1 && requests == 1); /* Inactive PC3 remains untouched. */
    active_handle = 3;
    settle();
    assert(requests == 2 && changed_handle == 2 && minimum_requested == 24 && latency_requested == 4);
    updated(2, 0, 24);
    settle();
    assert(requests == 3 && changed_handle == 3 && minimum_requested == 6 && maximum_requested == 6);
    updated(3, 0, 6);
    assert(hub_latency_set_fast(false) && !output_fast);
    settle();
    assert(updates == 2 && changed_handle == 1 && minimum_requested == 24);
    updated(1, 0, 24);
    settle();
    assert(requests == 4 && changed_handle == 3 && minimum_requested == 24);
}

static void test_refusal_timeout_and_pairing(void) {
    reset();
    connect_link(1, HCI_ROLE_MASTER);
    milliseconds = 2000;
    pairing_busy = true;
    assert(!hub_latency_set_fast(true));
    pairing_busy = false;
    assert(hub_latency_set_fast(true));
    request_error = ERROR_CODE_COMMAND_DISALLOWED;
    tick();
    assert(updates == 1 && !find_link(1)->pending);
    request_error = 0;
    for (unsigned i = 0; i < 80; ++i) tick();
    assert(updates == 1); /* No request storm when unsupported. */
    connect_link(2, HCI_ROLE_SLAVE);
    active_handle = 2;
    milliseconds += 2000;
    settle();
    assert(requests == 1 && find_link(2)->pending);
    for (unsigned i = 0; i < 25; ++i) tick();
    assert(!find_link(2)->pending && requests == 1);
    assert(perf_requests == 2 && perf_timeouts == 1);
    uint8_t gone[] = {HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0, 2, 0, 0x13};
    event_handler(HCI_EVENT_PACKET, 0, gone, sizeof(gone));
    assert(find_link(2) == NULL);
}

static void test_input_metrics_and_reset(void) {
    reset();
    microseconds = UINT32_MAX - 2000u;
    hub_latency_input_report(0, true);
    microseconds += 8000u;
    hub_latency_input_report(0, true);
    microseconds += 12000u;
    hub_latency_input_report(0, true);
    microseconds += 200000u;
    hub_latency_input_report(0, true);
    assert(timing[0][1].count == 4 && timing[0][1].samples == 2);
    assert(timing[0][1].minimum == 8000 && timing[0][1].maximum == 12000);
    assert(timing[0][1].sum == 20000 && timing[0][1].idle_gaps == 1);
    assert(perf_reports == 4);
    connect_link(2, HCI_ROLE_SLAVE);
    hub_latency_reset();
    assert(resets == 1 && timing[0][1].count == 0 && find_link(2) != NULL);
    assert(perf_resets == 1);
}

static void test_persisted_mode(void) {
    reset();
    assert(hub_latency_set_fast(true));
    hub_latency_init();
    assert(fast_mode && output_fast);
    assert(mouse_active_mode == !HUB_CLASSIC_OUTPUT_TEST);
    assert(hub_latency_set_fast(false));
    hub_latency_init();
    assert(!fast_mode && !output_fast);
    assert(!mouse_active_mode);
    fail_save = true;
    assert(!hub_latency_set_fast(true));
    assert(!fast_mode && !output_fast);
    fail_save = false;
    setting[5] ^= 1;
    hub_latency_init();
    assert(!fast_mode && !mouse_active_mode);
}

static void test_fast_skips_already_satisfied_parameters(void) {
    reset();
    connect_link(1, HCI_ROLE_MASTER);
    updated(1, 0, 6);
    connect_link(2, HCI_ROLE_SLAVE);
    updated(2, 0, 6);
    active_handle = 2;
    milliseconds = 2500;
    assert(hub_latency_set_fast(true));
    settle();
    assert(updates == 0 && requests == 0);
    assert(find_link(1)->satisfied == 1 && find_link(2)->satisfied == 1);
    assert(!find_link(1)->tuned && !find_link(2)->tuned);
    assert(hub_latency_set_fast(false));
    settle();
    assert(updates == 0 && requests == 0);
}

static void test_fast_boot_keeps_untuned_negotiation_as_baseline(void) {
    reset();
    assert(hub_latency_set_fast(true));
    hub_latency_init();
    working = true;
    connect_link(1, HCI_ROLE_MASTER);
    connect_link(2, HCI_ROLE_SLAVE);
    updated(1, 0, 6);
    updated(2, 0, 12);
    assert(find_link(1)->baseline_interval == 6);
    assert(find_link(2)->baseline_interval == 12);
    milliseconds = 2500;
    tick();
    assert(updates == 0 && find_link(1)->satisfied == 1);
    active_handle = 2;
    settle();
    assert(requests == 1 && minimum_requested == 6 && maximum_requested == 6);
    updated(2, 0, 6);
    assert(find_link(2)->baseline_interval == 12);
    assert(hub_latency_set_fast(false));
    settle();
    assert(updates == 0);
    assert(changed_handle == 2 && minimum_requested == 12 && maximum_requested == 12);
}

static void test_pc_can_refuse_exact_interval_without_retries(void) {
    reset();
    connect_link(2, HCI_ROLE_SLAVE);
    updated(2, 0, 12);
    active_handle = 2;
    milliseconds = 2500;
    assert(hub_latency_set_fast(true));
    settle();
    assert(requests == 1 && minimum_requested == 6 && maximum_requested == 6);
    uint8_t refused[] = {L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_RESPONSE, 4, 2, 0, 1, 0};
    event_handler(HCI_EVENT_PACKET, 0, refused, sizeof(refused));
    for (unsigned i = 0; i < 80; ++i) tick();
    assert(requests == 1 && !find_link(2)->pending);
    assert(find_link(2)->interval == 12 && find_link(2)->baseline_interval == 12);
    assert(hub_latency_set_fast(false));
    settle();
    assert(requests == 1 && find_link(2)->satisfied == 1 && !find_link(2)->tuned);
}

static void test_busy_input_and_encryption_delay_updates(void) {
    reset();
    connect_link(2, HCI_ROLE_SLAVE);
    active_handle = 2;
    assert(hub_latency_set_fast(true));
    input_busy = true;
    for (unsigned i = 0; i < 40; ++i) tick();
    assert(requests == 0);
    input_busy = false;
    for (unsigned i = 0; i < 7; ++i) tick();
    assert(requests == 0);
    tick();
    assert(requests == 1);

    reset();
    connect_link(1, HCI_ROLE_MASTER);
    connect_link(2, HCI_ROLE_SLAVE);
    active_handle = 2;
    unencrypted_handle = 1;
    assert(hub_latency_set_fast(true));
    settle();
    assert(!updates && !requests);
    unencrypted_handle = HCI_CON_HANDLE_INVALID;
    settle();
    assert(updates == 1 && requests == 0);
}

static void test_all_links_wait_for_pending_request_and_quiet_window(void) {
    reset();
    connect_link(1, HCI_ROLE_MASTER);
    connect_link(2, HCI_ROLE_SLAVE);
    active_handle = 2;
    assert(hub_latency_set_fast(true));
    settle();
    assert(updates == 1 && requests == 0);
    for (unsigned i = 0; i < 12; ++i) tick();
    assert(requests == 0 && find_link(1)->pending);
    updated(1, 0, 6);
    for (unsigned i = 0; i < 7; ++i) tick();
    assert(requests == 0);
    tick();
    assert(requests == 1);
}

static void test_peer_updates_block_requests_and_preserve_fast_mouse(void) {
    reset();
    connect_link(1, HCI_ROLE_MASTER);
    uint8_t remote[] = {L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_REQUEST, 10,
                        1, 0, 6, 0, 6, 0, 44, 0, 216, 0};
    event_handler(HCI_EVENT_PACKET, 0, remote, sizeof(remote));
    assert(hub_latency_set_fast(true));
    settle();
    assert(updates == 0 && find_link(1)->remote_pending);
    uint8_t applied[] = {HCI_EVENT_LE_META, 10, HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE,
                         0, 1, 0, 6, 0, 44, 0, 216, 0};
    event_handler(HCI_EVENT_PACKET, 0, applied, sizeof(applied));
    settle();
    assert(updates == 1 && minimum_requested == 6 && maximum_requested == 6);
    assert(latency_requested == 0);
    updated(1, 0, 6);
    event_handler(HCI_EVENT_PACKET, 0, applied, sizeof(applied));
    for (unsigned i = 0; i < 40; ++i) tick();
    assert(updates == (HUB_CLASSIC_OUTPUT_TEST ? 2u : 1u));
}

#if HUB_CLASSIC_OUTPUT_TEST
static void test_zero_latency_guard_rejects_peer_power_saving(void) {
    reset();
    connect_link(1, HCI_ROLE_MASTER);
    assert(hub_latency_set_fast(true));
    assert(!range_writes); /* Preserve the initial interval negotiation. */
    settle();
    updated(1, 0, 9);
    assert(zero_latency_guard && parameter_range.le_conn_latency_max == 0);
    assert(parameter_range.le_conn_interval_min == 6 && parameter_range.le_conn_interval_max == 3200);
    assert(parameter_range.le_supervision_timeout_max == 3200);
    uint8_t l2cap[] = {L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_REQUEST, 10,
                       1, 0, 6, 0, 9, 0, 44, 0, 216, 0};
    uint8_t hci[] = {HCI_EVENT_LE_META, 11, HCI_SUBEVENT_LE_REMOTE_CONNECTION_PARAMETER_REQUEST,
                     1, 0, 6, 0, 9, 0, 44, 0, 216, 0};
    event_handler(HCI_EVENT_PACKET, 0, l2cap, sizeof(l2cap));
    event_handler(HCI_EVENT_PACKET, 0, hci, sizeof(hci));
    assert(rejected_latency_requests == 2 && !find_link(1)->remote_pending);
    for (unsigned i = 0; i < 160; ++i) tick();
    assert(updates == 1 && !perf_timeouts && range_writes == 1);
    hub_latency_reset();
    assert(zero_latency_guard && !rejected_latency_requests);
    l2cap[8] = 0;
    event_handler(HCI_EVENT_PACKET, 0, l2cap, sizeof(l2cap));
    assert(find_link(1)->remote_pending); /* Valid latency=0 updates still serialize. */
    updated(1, 0, 9);
    fail_save = true;
    assert(!hub_latency_set_fast(false) && zero_latency_guard);
    fail_save = false;
    assert(hub_latency_set_fast(false) && !zero_latency_guard);
    assert(parameter_range.le_conn_latency_max == 500 && range_writes == 2);
    assert(hub_latency_set_fast(true) && zero_latency_guard);
    uint8_t gone[] = {HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0, 1, 0, 0x13};
    event_handler(HCI_EVENT_PACKET, 0, gone, sizeof(gone));
    assert(!zero_latency_guard && parameter_range.le_conn_latency_max == 500);
}

static void test_input_zero_latency_reassertion_is_bounded(void) {
    reset();
    connect_link(1, HCI_ROLE_MASTER);
    assert(hub_latency_set_fast(true));
    settle();
    updated(1, 0, 9);
    uint32_t first_request = find_link(1)->request_at;
    uint8_t applied[] = {HCI_EVENT_LE_META, 10, HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE,
                         0, 1, 0, 9, 0, 44, 0, 216, 0};
    event_handler(HCI_EVENT_PACKET, 0, applied, sizeof(applied));
    assert(find_link(1)->reassert_zero);
    while ((uint32_t)(milliseconds - first_request) < LATENCY_INPUT_REASSERT_MS - 250u) tick();
    assert(updates == 1);
    input_busy = true;
    tick();
    assert(updates == 1);
    input_busy = false;
    for (unsigned i = 0; i < 7; ++i) tick();
    assert(updates == 1);
    request_error = ERROR_CODE_COMMAND_DISALLOWED;
    tick();
    assert(updates == 2 && latency_requested == 0 && maximum_requested == 9);
    assert(!find_link(1)->reassert_zero);
    for (unsigned i = 0; i < 120; ++i) tick();
    assert(updates == 2); /* A rejected request does not become a retry storm. */
    request_error = 0;
    event_handler(HCI_EVENT_PACKET, 0, applied, sizeof(applied));
    settle();
    assert(updates == 3 && find_link(1)->pending);
    updated(1, 0, 9);
    assert(!find_link(1)->reassert_zero);
    assert(hub_latency_set_fast(false));
    settle();
    unsigned before = updates;
    event_handler(HCI_EVENT_PACKET, 0, applied, sizeof(applied));
    for (unsigned i = 0; i < 80; ++i) tick();
    assert(updates == before); /* Normal mode allows the saved low-power policy. */
}
#endif

static void test_quiet_window_wrap_and_measurement_reset(void) {
    reset();
    milliseconds = UINT32_MAX - 1000u;
    connect_link(1, HCI_ROLE_MASTER);
    assert(hub_latency_set_fast(true));
    for (unsigned i = 0; i < 7; ++i) tick();
    assert(updates == 0);
    tick();
    assert(updates == 1 && find_link(1)->pending);
    uint32_t activity = radio_activity_at;
    hub_latency_reset();
    assert(find_link(1)->pending && radio_activity_at == activity);
    for (unsigned i = 0; i < 25; ++i) tick();
    assert(updates == 1 && !find_link(1)->pending && perf_timeouts == 1);
}

static void test_peer_timeout_and_disconnect_release_global_guard(void) {
    reset();
    connect_link(1, HCI_ROLE_MASTER);
    connect_link(2, HCI_ROLE_SLAVE);
    active_handle = 2;
    uint8_t remote[] = {HCI_EVENT_LE_META, 11, HCI_SUBEVENT_LE_REMOTE_CONNECTION_PARAMETER_REQUEST,
                        1, 0, 6, 0, 6, 0, 44, 0, 216, 0};
    event_handler(HCI_EVENT_PACKET, 0, remote, sizeof(remote));
    assert(find_link(1)->remote_pending);
    assert(hub_latency_set_fast(true));
    for (unsigned i = 0; i < 27; ++i) tick();
    assert(!updates && !requests && !find_link(1)->remote_pending);
    tick();
    assert(updates == 1 && find_link(1)->pending);
    uint8_t gone[] = {HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0, 1, 0, 0x08};
    event_handler(HCI_EVENT_PACKET, 0, gone, sizeof(gone));
    for (unsigned i = 0; i < 7; ++i) tick();
    assert(requests == 0);
    tick();
    assert(requests == 1 && changed_handle == 2);
}

static void test_satisfied_pc_does_not_restore_or_repeat_on_switch(void) {
    reset();
    connect_link(2, HCI_ROLE_SLAVE);
    connect_link(3, HCI_ROLE_SLAVE);
    updated(2, 0, 6);
    updated(3, 0, 6);
    active_handle = 2;
    assert(hub_latency_set_fast(true));
    settle();
    for (unsigned i = 0; i < 6; ++i) {
        active_handle = i % 2 ? 2 : 3;
        settle();
    }
    assert(requests == 0 && updates == 0);
    assert(!find_link(2)->tuned && !find_link(3)->tuned);
}

#if HUB_CLASSIC_MOUSE_7P5_TEST
static void test_mouse_interval_only(void) {
    reset();
    connect_link(1, HCI_ROLE_MASTER);
    connect_link(2, HCI_ROLE_SLAVE);
    active_handle = 2;
    uint8_t applied[] = {HCI_EVENT_LE_META, 10, HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE,
                         0, 1, 0, 9, 0, 44, 0, 216, 0};
    event_handler(HCI_EVENT_PACKET, 0, applied, sizeof(applied));
    settle();
    assert(!updates && !requests); /* Not a ready mouse yet. */
    mouse_handle = 1;
    input_busy = true;
    settle();
    assert(!updates);
    input_busy = false;
    unencrypted_handle = 1;
    settle();
    assert(!updates);
    unencrypted_handle = HCI_CON_HANDLE_INVALID;
    settle();
    assert(updates == 1 && !requests && changed_handle == 1);
    assert(minimum_requested == 6 && maximum_requested == 6);
    assert(latency_requested == 44 && timeout_requested == 216);
    assert(!fast_mode && !output_fast && !zero_latency_guard && !range_writes);
    applied[6] = 6;
    event_handler(HCI_EVENT_PACKET, 0, applied, sizeof(applied));
    uint32_t first_request = find_link(1)->request_at;
    hub_latency_reset();
    settle();
    assert(updates == 1 && find_link(1)->interval == 6);
    applied[6] = 9;
    event_handler(HCI_EVENT_PACKET, 0, applied, sizeof(applied));
    while ((uint32_t)(milliseconds - first_request) < LATENCY_INPUT_REASSERT_MS - 250u) tick();
    assert(updates == 1);
    request_error = ERROR_CODE_COMMAND_DISALLOWED;
    tick();
    assert(updates == 2 && !find_link(1)->pending);
    for (unsigned i = 0; i < 100; ++i) tick();
    assert(updates == 2); /* Rejection alone never starts a retry storm. */
    request_error = 0;
    event_handler(HCI_EVENT_PACKET, 0, applied, sizeof(applied));
    settle();
    assert(updates == 3 && find_link(1)->pending);
    for (unsigned i = 0; i < 100; ++i) tick();
    assert(updates == 3 && perf_timeouts == 1);
    uint8_t gone[] = {HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0, 1, 0, 0x13};
    event_handler(HCI_EVENT_PACKET, 0, gone, sizeof(gone));
    connect_link(1, HCI_ROLE_MASTER);
    applied[6] = 6;
    event_handler(HCI_EVENT_PACKET, 0, applied, sizeof(applied));
    settle();
    assert(updates == 3 && find_link(1)->satisfied == 1); /* Already at 7.5ms. */
    assert(!requests && !zero_latency_guard);

    assert(hub_latency_set_fast(true));
    settle();
    assert(updates == 4 && latency_requested == 0);
    updated(1, 0, 6);
    active_handle = HCI_CON_HANDLE_INVALID;
    assert(hub_latency_set_fast(false));
    settle();
    assert(updates == 5 && minimum_requested == 6 && maximum_requested == 6);
    assert(latency_requested == 44 && timeout_requested == 216 && !zero_latency_guard);
}
#endif

static void test_mouse_active_default_and_support(void) {
    reset();
    assert(!mouse_active_mode);
    assert(!hub_latency_set_mouse_active(true));
    assert(hub_latency_set_fast(true));
    pairing_busy = true;
    assert(!hub_latency_set_mouse_active(true) && !mouse_active_mode);
    pairing_busy = false;
    assert(hub_latency_set_mouse_active(true) == !HUB_CLASSIC_OUTPUT_TEST);
    assert(mouse_active_mode == !HUB_CLASSIC_OUTPUT_TEST);
    uint8_t saved[sizeof(setting)];
    memcpy(saved, setting, sizeof(saved));
    hub_latency_mouse_active_status();
    assert(!memcmp(saved, setting, sizeof(saved)) && !updates && !requests);
    hub_latency_init();
    assert(fast_mode && mouse_active_mode == !HUB_CLASSIC_OUTPUT_TEST && !updates && !requests);
    assert(!pc_standby_mode && !memcmp(saved, setting, sizeof(saved)));
}

#if !HUB_CLASSIC_OUTPUT_TEST
static void test_mouse_active_fast_boot_default_and_runtime_off(void) {
    reset();
    assert(hub_latency_set_fast(true));
    assert(hub_latency_set_mouse_active(false));
    uint8_t saved[sizeof(setting)];
    memcpy(saved, setting, sizeof(saved));
    hub_latency_init();
    assert(mouse_active_mode && fast_mode && !pc_standby_mode && !updates && !requests);
    assert(!memcmp(saved, setting, sizeof(saved)));
    working = true;
    mouse_handle = 1;
    connect_link(1, HCI_ROLE_MASTER);
    updated(1, 0, 6);
    settle();
    assert(!updates && !requests); /* Boot opt-in does not force an already-fast link. */
    updated_parameters(1, 0, 6, 44, 216);
    settle();
    assert(!updates && !find_link(1)->mouse_activity_pending);
    hub_latency_input_report(0, false);
    settle();
    assert(!updates); /* Keyboard traffic is not mouse activity. */
    hub_latency_mouse_activity(1);
    tick();
    assert(updates == 1 && changed_handle == 1 && find_link(1)->mouse_active_requests == 1);
    assert(minimum_requested == 6 && maximum_requested == 6);
    assert(latency_requested == 0 && timeout_requested == 216);
    updated_parameters(1, 0, 6, 0, 216);
    assert(hub_latency_set_mouse_active(false));
    milliseconds += 30000;
    updated_parameters(1, 0, 6, 44, 216);
    for (unsigned i = 0; i < 80; ++i) {
        hub_latency_mouse_activity(1);
        tick();
    }
    assert(updates == 1 && !mouse_active_mode);
    hub_latency_reset();
    assert(!mouse_active_mode); /* Measurement reset does not re-enable the feature. */
    hub_latency_init();
    assert(mouse_active_mode && fast_mode && !pc_standby_mode);
    assert(!memcmp(saved, setting, sizeof(saved)));
    assert(hub_latency_set_fast(false) && !mouse_active_mode);
    hub_latency_init();
    assert(!fast_mode && !mouse_active_mode);
}

static void setup_active_mouse(uint16_t interval) {
    reset();
    mouse_handle = 1;
    connect_link(1, HCI_ROLE_MASTER);
    assert(hub_latency_set_fast(true));
    settle();
    assert(updates == 1);
    updated(1, 0, 6);
    milliseconds += 30000;
    updated_parameters(1, 0, interval, 44, 216);
    settle();
    assert(updates == 1 && !find_link(1)->pending);
}

static void mouse_tick(void) {
    hub_latency_mouse_activity(1);
    tick();
}

static void test_mouse_active_targeting_and_exact_parameters(void) {
    const uint16_t intervals[] = {6, 9, 12, 24};
    for (unsigned n = 0; n < sizeof(intervals) / sizeof(intervals[0]); ++n) {
        setup_active_mouse(intervals[n]);
        hub_latency_mouse_activity(1);
        settle();
        assert(updates == 1 && !find_link(1)->mouse_activity_pending);
        assert(hub_latency_set_mouse_active(true));
        settle();
        assert(updates == 1); /* Opt-in alone is not activity. */
        connect_link(2, HCI_ROLE_SLAVE);
        updated(2, 0, 6);
        active_handle = 2;
        connect_link(3, HCI_ROLE_MASTER);
        settle();
        hub_latency_mouse_activity(HCI_CON_HANDLE_INVALID);
        hub_latency_mouse_activity(3);
        mouse_handle = 2; /* Even a misidentified PC cannot be targeted. */
        hub_latency_mouse_activity(2);
        mouse_handle = HCI_CON_HANDLE_INVALID;
        hub_latency_mouse_activity(1);
        mouse_handle = 1;
        hub_latency_input_report(0, false);
        settle();
        assert(updates == 1 && !requests);
        for (unsigned i = 0; i < 1000; ++i) hub_latency_mouse_activity(1);
        assert(updates == 1 && find_link(1)->mouse_activity_pending);
        tick();
        assert(updates == 2 && !requests && changed_handle == 1);
        assert(minimum_requested == intervals[n] && maximum_requested == intervals[n]);
        assert(latency_requested == 0 && timeout_requested == 216 && !range_writes);
        assert(find_link(1)->pending && find_link(1)->mouse_active_requests == 1);
        for (unsigned i = 0; i < 8; ++i) mouse_tick();
        assert(updates == 2);
        updated_parameters(1, 0, intervals[n], 0, 216);
        assert(!find_link(1)->mouse_activity_pending);
        for (unsigned i = 0; i < 80; ++i) mouse_tick();
        assert(updates == 2 && !requests && !range_writes);
    }
}

static void test_mouse_active_idle_expiry_and_peer_power_saving(void) {
    setup_active_mouse(6);
    assert(hub_latency_set_mouse_active(true));
    hub_latency_mouse_activity(1);
    milliseconds += LATENCY_MOUSE_ACTIVITY_MS + 1;
    tick();
    assert(updates == 1 && !find_link(1)->mouse_activity_pending);
    hub_latency_mouse_activity(1);
    input_busy = true;
    settle();
    input_busy = false;
    settle();
    assert(updates == 1); /* Stale activity does not survive a busy period. */
    mouse_tick();
    assert(updates == 2);
    updated_parameters(1, 0, 6, 0, 216);
    milliseconds += 30000;
    uint8_t remote[] = {L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_REQUEST, 10,
                        1, 0, 6, 0, 9, 0, 44, 0, 216, 0};
    event_handler(HCI_EVENT_PACKET, 0, remote, sizeof(remote));
    assert(find_link(1)->remote_pending && !range_writes);
    updated_parameters(1, 0, 6, 44, 216);
    for (unsigned i = 0; i < 240; ++i) tick();
    assert(updates == 2 && find_link(1)->latency == 44);
    mouse_tick();
    assert(updates == 3 && latency_requested == 0 && timeout_requested == 216);
}

static void test_mouse_active_serialization_and_security(void) {
    setup_active_mouse(6);
    assert(hub_latency_set_mouse_active(true));
    connect_link(2, HCI_ROLE_SLAVE);
    uint8_t remote[] = {L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_REQUEST, 10,
                        2, 0, 6, 0, 6, 0, 0, 0, 200, 0};
    event_handler(HCI_EVENT_PACKET, 0, remote, sizeof(remote));
    for (unsigned i = 0; i < 12; ++i) mouse_tick();
    assert(updates == 1 && find_link(2)->remote_pending);
    updated(2, 0, 6);
    for (unsigned i = 0; i < 7; ++i) mouse_tick();
    assert(updates == 1);
    pairing_busy = true;
    for (unsigned i = 0; i < 12; ++i) mouse_tick();
    assert(updates == 1);
    pairing_busy = false;
    unencrypted_handle = 1;
    for (unsigned i = 0; i < 12; ++i) mouse_tick();
    assert(updates == 1);
    unencrypted_handle = HCI_CON_HANDLE_INVALID;
    for (unsigned i = 0; i < 7; ++i) mouse_tick();
    assert(updates == 1);
    mouse_tick();
    assert(updates == 2 && !requests && changed_handle == 1);
}

static void test_mouse_active_failures_and_cooldown(void) {
    setup_active_mouse(9);
    assert(hub_latency_set_mouse_active(true));
    request_error = ERROR_CODE_COMMAND_DISALLOWED;
    mouse_tick();
    assert(updates == 2 && !find_link(1)->pending);
    uint32_t first = find_link(1)->request_at;
    request_error = 0;
    while ((uint32_t)(milliseconds - first) < LATENCY_INPUT_REASSERT_MS - 250u) mouse_tick();
    assert(updates == 2);
    mouse_tick();
    assert(updates == 3 && find_link(1)->pending);
    assert((uint32_t)(milliseconds - first) == LATENCY_INPUT_REASSERT_MS);
    updated_parameters(1, ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE, 9, 0, 216);
    assert(!find_link(1)->pending && find_link(1)->latency == 44);
    for (unsigned i = 0; i < 80; ++i) tick();
    assert(updates == 3); /* A failure alone never retries. */
    mouse_tick();
    assert(updates == 4 && find_link(1)->pending);
    first = find_link(1)->request_at;
    while ((uint32_t)(milliseconds - first) < LATENCY_INPUT_REASSERT_MS - 250u) mouse_tick();
    assert(updates == 4 && !find_link(1)->pending && perf_timeouts == 1);
    mouse_tick();
    assert(updates == 5 && find_link(1)->pending);
    assert(perf_requests == updates);
}

static void test_mouse_active_switches_and_measurement_reset(void) {
    setup_active_mouse(6);
    assert(hub_latency_set_mouse_active(true));
    hub_latency_mouse_activity(1);
    assert(hub_latency_set_mouse_active(false));
    tick();
    assert(updates == 1 && !find_link(1)->mouse_activity_pending);
    assert(hub_latency_set_mouse_active(true));
    tick();
    assert(updates == 1);
    mouse_tick();
    assert(updates == 2 && find_link(1)->pending);
    hub_latency_mouse_activity(1);
    uint32_t requested = find_link(1)->request_at;
    uint32_t activity = find_link(1)->mouse_activity_at;
    hub_latency_reset();
    assert(mouse_active_mode && find_link(1)->pending && find_link(1)->mouse_activity_pending);
    assert(find_link(1)->request_at == requested && find_link(1)->mouse_activity_at == activity);
    assert(find_link(1)->requests == 2 && !find_link(1)->mouse_active_requests);
    assert(hub_latency_set_mouse_active(false) && find_link(1)->pending);
    updated_parameters(1, 0, 6, 0, 216);
    assert(find_link(1)->latency == 0); /* OFF does not undo an in-flight update. */
    milliseconds += 30000;
    updated_parameters(1, 0, 6, 44, 216);
    for (unsigned i = 0; i < 80; ++i) mouse_tick();
    assert(updates == 2);
    assert(hub_latency_set_mouse_active(true));
    hub_latency_mouse_activity(1);
    fail_save = true;
    assert(!hub_latency_set_fast(false) && mouse_active_mode);
    fail_save = false;
    assert(hub_latency_set_fast(false) && !mouse_active_mode);
    assert(!find_link(1)->mouse_activity_pending);
    settle();
    assert(updates == 3 && minimum_requested == 24 && maximum_requested == 24);
    assert(latency_requested == 4 && timeout_requested == 200);
    assert(hub_latency_set_fast(true) && !mouse_active_mode);
}

static void test_mouse_active_wrap_and_handle_reuse(void) {
    setup_active_mouse(6);
    assert(hub_latency_set_mouse_active(true));
    milliseconds = UINT32_MAX - 100u;
    radio_activity_at = milliseconds - LATENCY_SETTLE_MS;
    find_link(1)->request_at = milliseconds - LATENCY_INPUT_REASSERT_MS;
    mouse_tick();
    assert(updates == 2 && find_link(1)->pending);
    updated_parameters(1, 0, 6, 0, 216);
    milliseconds += 30000;
    updated_parameters(1, 0, 6, 44, 216);
    hub_latency_mouse_activity(1);
    uint8_t gone[] = {HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0, 1, 0, 0x13};
    event_handler(HCI_EVENT_PACKET, 0, gone, sizeof(gone));
    connect_link(1, HCI_ROLE_MASTER);
    updated(1, 0, 6);
    settle();
    assert(updates == 2 && !find_link(1)->mouse_activity_pending);
    assert(!find_link(1)->mouse_active_requests);
    updated_parameters(1, 0, 6, 44, 216);
    settle();
    assert(updates == 2);
    mouse_tick();
    assert(updates == 3 && find_link(1)->mouse_active_requests == 1);
}
#endif

static void test_pc_standby_default_and_support(void) {
    reset();
    assert(!pc_standby_mode && !hub_latency_set_pc_standby(true));
    assert(hub_latency_set_fast(true));
    pairing_busy = true;
    assert(!hub_latency_set_pc_standby(true) && !pc_standby_mode);
    pairing_busy = false;
    uint8_t saved[sizeof(setting)];
    memcpy(saved, setting, sizeof(saved));
    assert(hub_latency_set_pc_standby(true) == !HUB_CLASSIC_OUTPUT_TEST);
    hub_latency_pc_standby_status();
    assert(!updates && !requests && !memcmp(saved, setting, sizeof(saved)));
    hub_latency_reset();
    assert(pc_standby_mode == !HUB_CLASSIC_OUTPUT_TEST);
    hub_latency_init();
    assert(fast_mode && !pc_standby_mode);
}

#if !HUB_CLASSIC_OUTPUT_TEST
static void setup_standby_pcs(void) {
    reset();
    connect_link(1, HCI_ROLE_MASTER);
    updated(1, 0, 6);
    connect_link(2, HCI_ROLE_SLAVE);
    updated(2, 0, 6);
    connect_link(3, HCI_ROLE_SLAVE);
    updated(3, 0, 6);
    active_handle = 2;
    assert(hub_latency_set_fast(true));
    settle();
    assert(!updates && !requests);
    assert(hub_latency_set_pc_standby(true));
}

static void test_pc_standby_four_pcs_and_promotion_priority(void) {
    setup_standby_pcs();
    for (unsigned handle = 4; handle <= 5; ++handle) {
        connect_link(handle, HCI_ROLE_SLAVE);
        updated(handle, 0, 6);
    }
    connect_link(6, HCI_ROLE_SLAVE); /* Not a registered ready output. */
    connect_link(7, HCI_ROLE_MASTER); /* Not an input or a PC. */
    for (unsigned handle = 3; handle <= 5; ++handle) {
        settle();
        assert(requests == handle - 2 && changed_handle == handle && !updates);
        assert(minimum_requested == 12 && maximum_requested == 12);
        assert(latency_requested == 0 && timeout_requested == 200);
        assert(find_link(handle)->pending && find_link(handle)->tuned);
        for (unsigned i = 0; i < 8; ++i) tick();
        assert(requests == handle - 2); /* Globally serialized. */
        updated(handle, 0, 12);
        assert(find_link(handle)->baseline_interval == 6);
    }
    settle();
    assert(requests == 3 && !find_link(2)->tuned && !find_link(6)->tuned);
    assert(find_link(1)->interval == 6 && !range_writes);
    active_handle = 3;
    settle();
    assert(requests == 4 && changed_handle == 3 && minimum_requested == 6);
    assert(!find_link(2)->pending && !find_link(3)->requested_standby);
    updated(3, 0, 6);
    settle();
    assert(requests == 5 && changed_handle == 2 && minimum_requested == 12);
    updated(2, 0, 12);
    assert(hub_latency_set_pc_standby(false));
    const uint16_t restoring[] = {2, 4, 5};
    for (unsigned i = 0; i < 3; ++i) {
        settle();
        assert(requests == i + 6 && changed_handle == restoring[i] && minimum_requested == 6);
        assert(find_link(restoring[i])->tuned); /* Still protected while restoration is pending. */
        updated(restoring[i], 0, 6);
        assert(!find_link(restoring[i])->tuned);
    }
    settle();
    assert(requests == 8 && !updates && fast_mode && find_link(3)->interval == 6);
}

static void test_pc_standby_busy_readiness_and_no_active_target(void) {
    setup_standby_pcs();
    not_ready_handle = 3;
    settle();
    assert(!requests);
    not_ready_handle = HCI_CON_HANDLE_INVALID;
    active_handle = HCI_CON_HANDLE_INVALID; /* Release barrier or selected PC offline. */
    settle();
    assert(!requests);
    active_handle = 2;
    input_busy = true;
    settle();
    input_busy = false;
    pairing_busy = true;
    settle();
    pairing_busy = false;
    unencrypted_handle = 3;
    settle();
    assert(!requests);
    unencrypted_handle = HCI_CON_HANDLE_INVALID;
    for (unsigned i = 0; i < 7; ++i) tick();
    assert(!requests);
    tick();
    assert(requests == 1 && changed_handle == 3);
    updated(3, 0, 12);
    active_handle = HCI_CON_HANDLE_INVALID;
    settle();
    assert(requests == 1); /* No transient restore while switching. */
    assert(hub_latency_set_pc_standby(false));
    settle();
    assert(requests == 2 && changed_handle == 3 && minimum_requested == 6);
}

static void test_pc_standby_refusal_timeout_and_peer_override(void) {
    for (unsigned failure = 0; failure < 5; ++failure) {
        setup_standby_pcs();
        if (failure == 0) request_error = ERROR_CODE_COMMAND_DISALLOWED;
        settle();
        assert(requests == 1 && changed_handle == 3);
        request_error = 0;
        if (failure == 1) {
            uint8_t refused[] = {L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_RESPONSE, 4, 3, 0, 1, 0};
            event_handler(HCI_EVENT_PACKET, 0, refused, sizeof(refused));
        } else if (failure == 2) {
            updated(3, ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE, 12);
        } else if (failure == 3) {
            updated(3, 0, 24); /* An applied update is not necessarily the requested value. */
        }
        for (unsigned i = 0; i < 80; ++i) tick();
        assert(requests == 1 && !find_link(3)->pending && !updates);
        assert(find_link(3)->baseline_interval == 6);
        assert(find_link(3)->interval == (failure == 3 ? 24 : 6));
        assert(perf_timeouts == (failure == 4 ? 1u : 0u));
        assert(hub_latency_set_pc_standby(false));
        settle();
        if (failure == 3) {
            assert(requests == 2 && minimum_requested == 6);
            updated(3, 0, 24); /* Refused/clamped restoration must not poison the baseline. */
            settle();
            assert(requests == 2 && find_link(3)->baseline_interval == 6 && find_link(3)->tuned);
        } else assert(requests == 1);
    }
}

static void test_pc_standby_pending_switch_and_disable(void) {
    setup_standby_pcs();
    settle();
    active_handle = 3;
    settle();
    assert(requests == 1 && find_link(3)->pending);
    updated(3, 0, 12);
    settle();
    assert(requests == 2 && changed_handle == 3 && minimum_requested == 6);
    active_handle = 2; /* Latest selection wins after the outstanding update. */
    updated(3, 0, 6);
    settle();
    assert(requests == 3 && changed_handle == 3 && minimum_requested == 12);
    assert(hub_latency_set_pc_standby(false));
    hub_latency_reset();
    assert(find_link(3)->pending && !pc_standby_mode && find_link(3)->tuned);
    settle();
    assert(requests == 3);
    updated(3, 0, 12);
    assert(find_link(3)->baseline_interval == 6);
    settle();
    assert(requests == 4 && changed_handle == 3 && minimum_requested == 6);
    updated(3, 0, 6);
    assert(!find_link(3)->tuned && find_link(3)->baseline_interval == 6);
}

static void test_pc_standby_timeout_bounds_normal_and_reuse(void) {
    setup_standby_pcs();
    updated_parameters(3, 0, 6, 0, 10);
    settle();
    assert(requests == 1 && minimum_requested == 12 && maximum_requested == 12);
    assert(timeout_requested == 10); /* The minimum legal 100ms timeout is valid at 15ms. */
    updated_parameters(3, 0, 12, 0, 10);
    assert(find_link(3)->baseline_timeout == 10);
    fail_save = true;
    assert(!hub_latency_set_fast(false) && pc_standby_mode);
    fail_save = false;
    assert(hub_latency_set_fast(false) && !pc_standby_mode);
    settle();
    assert(requests == 2 && minimum_requested == 6 && timeout_requested == 10);
    updated_parameters(3, 0, 6, 0, 10);
    assert(!find_link(3)->tuned);
    assert(hub_latency_set_fast(true));
    assert(hub_latency_set_pc_standby(true));
    milliseconds = UINT32_MAX - 1000u;
    radio_activity_at = milliseconds;
    for (unsigned i = 0; i < 7; ++i) tick();
    assert(requests == 2);
    tick();
    assert(requests == 3 && find_link(3)->pending);
    uint8_t gone[] = {HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0, 3, 0, 0x13};
    event_handler(HCI_EVENT_PACKET, 0, gone, sizeof(gone));
    connect_link(3, HCI_ROLE_SLAVE);
    assert(!find_link(3)->requested_standby && !find_link(3)->tuned);
    settle();
    assert(requests == 4 && minimum_requested == 12);
    updated(3, 0, 12);
    assert(find_link(3)->baseline_interval == 24);
    assert(hub_latency_set_pc_standby(false));
    settle();
    assert(requests == 5 && minimum_requested == 24 && latency_requested == 4);
}

static void test_pc_standby_already_15ms_and_selected_7p5ms(void) {
    setup_standby_pcs();
    updated_parameters(3, 0, 12, 0, 216);
    settle();
    assert(!requests && find_link(3)->requested_standby && !find_link(3)->tuned);
    assert(find_link(3)->interval == 12 && find_link(3)->baseline_interval == 12);
    active_handle = 3;
    settle();
    assert(requests == 1 && changed_handle == 3);
    assert(minimum_requested == 6 && maximum_requested == 6 && latency_requested == 0);
    updated(3, 0, 6);
    settle();
    assert(requests == 2 && changed_handle == 2);
    assert(minimum_requested == 12 && maximum_requested == 12 && latency_requested == 0);
    updated(2, 0, 12);
    assert(find_link(3)->interval == 6 && find_link(2)->interval == 12);
    assert(hub_latency_set_pc_standby(false));
    settle();
    assert(requests == 3 && changed_handle == 2 && minimum_requested == 6);
    updated(2, 0, 6);
    assert(hub_latency_set_fast(false));
    settle();
    assert(requests == 4 && changed_handle == 3);
    assert(minimum_requested == 12 && maximum_requested == 12 && timeout_requested == 216);
    updated_parameters(3, 0, 12, 0, 216);
    assert(!find_link(3)->tuned && !updates && !range_writes);
}
#endif

int main(void) {
    test_pc_standby_default_and_support();
#if !HUB_CLASSIC_OUTPUT_TEST
    test_pc_standby_four_pcs_and_promotion_priority();
    test_pc_standby_busy_readiness_and_no_active_target();
    test_pc_standby_refusal_timeout_and_peer_override();
    test_pc_standby_pending_switch_and_disable();
    test_pc_standby_timeout_bounds_normal_and_reuse();
    test_pc_standby_already_15ms_and_selected_7p5ms();
#endif
    test_mouse_active_default_and_support();
#if !HUB_CLASSIC_OUTPUT_TEST
    test_mouse_active_fast_boot_default_and_runtime_off();
    test_mouse_active_targeting_and_exact_parameters();
    test_mouse_active_idle_expiry_and_peer_power_saving();
    test_mouse_active_serialization_and_security();
    test_mouse_active_failures_and_cooldown();
    test_mouse_active_switches_and_measurement_reset();
    test_mouse_active_wrap_and_handle_reuse();
#endif
    test_targeted_fast_and_restore();
    test_refusal_timeout_and_pairing();
    test_input_metrics_and_reset();
    test_persisted_mode();
    test_fast_skips_already_satisfied_parameters();
    test_fast_boot_keeps_untuned_negotiation_as_baseline();
    test_pc_can_refuse_exact_interval_without_retries();
    test_busy_input_and_encryption_delay_updates();
    test_all_links_wait_for_pending_request_and_quiet_window();
    test_peer_updates_block_requests_and_preserve_fast_mouse();
    test_quiet_window_wrap_and_measurement_reset();
    test_peer_timeout_and_disconnect_release_global_guard();
    test_satisfied_pc_does_not_restore_or_repeat_on_switch();
#if HUB_CLASSIC_OUTPUT_TEST
    test_zero_latency_guard_rejects_peer_power_saving();
    test_input_zero_latency_reassertion_is_bounded();
#else
    assert(range_writes == 0);
#endif
#if HUB_CLASSIC_MOUSE_7P5_TEST
    test_mouse_interval_only();
#endif
    puts("All runtime BLE latency policy/telemetry tests passed.");
    return 0;
}
