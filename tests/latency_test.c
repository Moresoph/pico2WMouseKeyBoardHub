#include <assert.h>
#include <stdio.h>
#include "..\src\hub_latency.c"

static uint32_t milliseconds, microseconds;
static uint16_t active_handle;
static bool pairing_busy, input_busy, output_fast;
static uint16_t unencrypted_handle;
static unsigned resets, updates, requests;
static unsigned perf_reports, perf_resets, perf_requests, perf_timeouts;
static uint16_t changed_handle, minimum_requested, maximum_requested, latency_requested;
static int request_error;
static uint8_t setting[9];
static int setting_length;
static bool fail_save;

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
uint16_t hub_output_active_handle(void) { return active_handle; }
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
    maximum_requested = maximum; latency_requested = latency; (void)timeout;
    return request_error;
}
int gap_request_connection_parameter_update(hci_con_handle_t handle, uint16_t minimum,
                                             uint16_t maximum, uint16_t latency, uint16_t timeout) {
    ++requests; changed_handle = handle; minimum_requested = minimum;
    maximum_requested = maximum; latency_requested = latency; (void)timeout;
    return request_error;
}

static void reset(void) {
    milliseconds = microseconds = 0;
    active_handle = HCI_CON_HANDLE_INVALID;
    pairing_busy = input_busy = output_fast = false;
    unencrypted_handle = HCI_CON_HANDLE_INVALID;
    resets = updates = requests = 0;
    perf_reports = perf_resets = perf_requests = perf_timeouts = 0;
    request_error = 0;
    setting_length = 0;
    fail_save = false;
    hub_latency_init();
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

static void updated(uint16_t handle, uint8_t status, uint16_t interval) {
    uint8_t packet[12] = {HCI_EVENT_LE_META, 10, HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE};
    packet[3] = status;
    little_endian_store_16(packet, 4, handle);
    little_endian_store_16(packet, 6, interval);
    little_endian_store_16(packet, 8, 0);
    little_endian_store_16(packet, 10, 200);
    event_handler(HCI_EVENT_PACKET, 0, packet, sizeof(packet));
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
    assert(hub_latency_set_fast(false));
    hub_latency_init();
    assert(!fast_mode && !output_fast);
    fail_save = true;
    assert(!hub_latency_set_fast(true));
    assert(!fast_mode && !output_fast);
    fail_save = false;
    setting[5] ^= 1;
    hub_latency_init();
    assert(!fast_mode);
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
    assert(updates == 1); /* Do not fight the peer's later power-saving update. */
}

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

int main(void) {
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
    puts("All runtime BLE latency policy/telemetry tests passed.");
    return 0;
}
