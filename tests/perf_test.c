#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char printed[32768];
static unsigned printed_length, print_calls;
static uint32_t milliseconds, microseconds, print_delay_ms;
static int capture_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsnprintf(printed + printed_length, sizeof(printed) - printed_length, format, args);
    va_end(args);
    assert(result >= 0 && (unsigned)result < sizeof(printed) - printed_length);
    printed_length += (unsigned)result;
    ++print_calls;
    milliseconds += print_delay_ms;
    microseconds += print_delay_ms * 1000u;
    return result;
}

#define printf capture_printf
#include "..\src\hub_perf.c"
#undef printf

static unsigned registrations, l2cap_registrations, timer_sets, timer_adds;
static btstack_packet_callback_registration_t *registered;
static btstack_packet_callback_registration_t *l2cap_registered;
static hci_role_t classic_role;

uint32_t hub_latency_now_us(void) { return microseconds; }
uint32_t btstack_run_loop_get_time_ms(void) { return milliseconds; }
hci_role_t gap_get_role(hci_con_handle_t handle) { (void)handle; return classic_role; }
void hci_add_event_handler(btstack_packet_callback_registration_t *handler) {
    registered = handler;
    ++registrations;
}
void l2cap_add_event_handler(btstack_packet_callback_registration_t *handler) {
    l2cap_registered = handler;
    ++l2cap_registrations;
}
void btstack_run_loop_set_timer_handler(btstack_timer_source_t *source,
                                        void (*handler)(btstack_timer_source_t *)) {
    source->process = handler;
}
void btstack_run_loop_set_timer(btstack_timer_source_t *source, uint32_t delay) {
    assert(source == &perf_timer && delay == PERF_TIMER_MS);
    source->timeout = milliseconds + delay + 1u;
    ++timer_sets;
}
void btstack_run_loop_add_timer(btstack_timer_source_t *source) {
    assert(source == &perf_timer);
    ++timer_adds;
}

static void reset_test(void) {
    memset(perf_links, 0, sizeof(perf_links));
    next_generation = 0;
    perf_initialized = false;
    milliseconds = microseconds = print_delay_ms = 0;
    registrations = l2cap_registrations = timer_sets = timer_adds = 0;
    registered = l2cap_registered = NULL;
    classic_role = HCI_ROLE_MASTER;
    printed_length = print_calls = 0;
    printed[0] = 0;
    hub_perf_init();
    assert(registrations == 1 && timer_sets == 1 && timer_adds == 1);
    assert(registered && registered->callback == perf_hci_handler);
    assert(l2cap_registrations == 1 && l2cap_registered != registered);
    assert(l2cap_registered->callback == perf_hci_handler);
    assert(perf_timer.process == perf_service && timer_due == PERF_TIMER_MS + 1u);
    assert(print_calls == 0);
}

static void emit(uint8_t *packet, uint16_t length) {
    assert(registered);
    registered->callback(HCI_EVENT_PACKET, 0, packet, length);
}

static void advance_ms(uint32_t amount) {
    milliseconds += amount;
    microseconds += amount * 1000u;
}

static void connect_le(uint16_t handle, uint8_t role) {
    uint8_t packet[36] = {HCI_EVENT_META_GAP, 34, GAP_SUBEVENT_LE_CONNECTION_COMPLETE, 0};
    little_endian_store_16(packet, 4, handle);
    packet[6] = role;
    little_endian_store_16(packet, 26, 6);
    little_endian_store_16(packet, 28, 0);
    little_endian_store_16(packet, 30, 200);
    emit(packet, sizeof(packet));
}

static void connect_classic(uint16_t handle, uint8_t status, uint8_t type) {
    uint8_t packet[13] = {HCI_EVENT_CONNECTION_COMPLETE, 11};
    packet[2] = status;
    little_endian_store_16(packet, 3, handle);
    packet[11] = type;
    emit(packet, sizeof(packet));
}

static void disconnect_link(uint16_t handle, uint8_t status, uint8_t reason) {
    uint8_t packet[] = {HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0, 0, 0, 0};
    packet[2] = status;
    little_endian_store_16(packet, 3, handle);
    packet[5] = reason;
    emit(packet, sizeof(packet));
}

static void complete(uint16_t handle, uint16_t count) {
    uint8_t packet[] = {HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS, 5, 1, 0, 0, 0, 0};
    little_endian_store_16(packet, 3, handle);
    little_endian_store_16(packet, 5, count);
    emit(packet, sizeof(packet));
}

static const perf_event_t *last_control(void) {
    assert(control_count);
    return &control_history[(control_next + PERF_CONTROL_HISTORY - 1u) % PERF_CONTROL_HISTORY];
}

static const perf_event_t *last_slow(void) {
    assert(slow_count);
    return &slow_history[(slow_next + PERF_SLOW_HISTORY - 1u) % PERF_SLOW_HISTORY];
}

static void test_transport_lifecycle_and_reuse(void) {
    reset_test();
    connect_classic(1, 0x04, 1);
    connect_classic(1, 0, 0); /* SCO is not an input ACL. */
    assert(!perf_find_link(1) && !control_count);
    connect_classic(1, 0, 1);
    perf_link_t *link = perf_find_link(1);
    assert(link && link->transport == PERF_CLASSIC && link->role == HCI_ROLE_MASTER);
    uint32_t first_generation = link->generation;
    connect_le(2, HCI_ROLE_MASTER);
    connect_le(3, HCI_ROLE_SLAVE);
    assert(perf_find_link(2)->transport == PERF_LE);
    assert(perf_find_link(3)->interval == 6 && perf_find_link(3)->timeout == 200);
    assert(perf_find_link(2)->generation != first_generation);
    assert(totals.connects == 3 && totals.input_boundaries == 2);
    hub_perf_input_report(0, true);
    assert(perf_inputs[0][1].clock.valid);
    disconnect_link(3, 0x0c, 0x28);
    assert(perf_find_link(3) && totals.disconnects == 0 && !totals.instant_passed);
    assert(last_control()->status == 0x0c);
    disconnect_link(3, 0, 0x28);
    assert(!perf_find_link(3) && totals.disconnects == 1 && totals.instant_passed == 1);
    assert(last_control()->count == 0x28 && last_control()->transport == PERF_LE);
    assert(perf_inputs[0][1].clock.valid); /* PC loss doesn't interrupt input timing. */
    complete(1, 4);
    disconnect_link(1, 0, 0x13);
    assert(!perf_find_link(1) && !perf_inputs[0][1].clock.valid);
    assert(last_control()->generation == first_generation && last_control()->value == 4);
    complete(1, 2);
    assert(totals.unknown_packets == 2);
    connect_le(1, HCI_ROLE_SLAVE);
    link = perf_find_link(1);
    assert(link && link->generation != first_generation && !link->credits.events);
    assert(!link->credits.clock.valid && link->output_slot == PERF_NO_SLOT);
    complete(1, 1);
    assert(link->credits.packets == 1 && link->credits.gaps.count == 0);
    disconnect_link(0x55, 0, 0x08);
    assert(last_control()->generation == 0 && last_control()->count == 8);
    assert(!print_calls);
}

static void test_classic_role_change_and_full_table(void) {
    reset_test();
    classic_role = HCI_ROLE_SLAVE;
    connect_classic(1, 0, 1);
    assert(perf_find_link(1)->role == HCI_ROLE_SLAVE);
    uint8_t role[] = {HCI_EVENT_ROLE_CHANGE, 8, 0, 0, 0, 0, 0, 0, 0, HCI_ROLE_MASTER};
    classic_role = HCI_ROLE_MASTER;
    emit(role, sizeof(role));
    assert(last_control()->kind == PERF_ROLE && last_control()->role == HCI_ROLE_MASTER);
    for (unsigned i = 2; i <= MAX_NR_HCI_CONNECTIONS; ++i) connect_le((uint16_t)i, HCI_ROLE_SLAVE);
    connect_le(0x123, HCI_ROLE_SLAVE);
    assert(totals.table_full == 1 && !perf_find_link(0x123));
    uint32_t generation = perf_find_link(1)->generation;
    disconnect_link(1, 0, 0x13);
    connect_le(0x123, HCI_ROLE_SLAVE);
    assert(perf_find_link(0x123) && perf_find_link(0x123)->generation > generation);
    assert(!print_calls);
}

static void test_batched_completions_and_malformed_events(void) {
    reset_test();
    connect_classic(1, 0, 1);
    connect_le(2, HCI_ROLE_SLAVE);
    uint8_t packet[] = {HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS, 13, 3,
                        1, 0, 2, 0, 2, 0, 0xff, 0xff, 0x55, 0, 7, 0};
    emit(packet, sizeof(packet));
    assert(totals.credit_events == 1 && totals.credit_entries == 3);
    assert(totals.credit_packets == 65544 && totals.unknown_packets == 7);
    assert(perf_find_link(1)->credits.packets == 2);
    assert(perf_find_link(2)->credits.packets == 65535);
    assert(perf_find_link(2)->credits.gaps.count == 0);
    assert(last_slow()->handle == 0x55 && last_slow()->generation == 0);
    advance_ms(30);
    complete(2, 3);
    assert(perf_find_link(2)->credits.gaps.last == 30000);
    assert(perf_find_link(2)->credits.last_batch == 3);
    assert(last_slow()->kind == PERF_CREDITS && last_slow()->count == 3);
    assert(last_slow()->value == 30000);
    complete(1, 0);
    assert(perf_find_link(1)->credits.packets == 2);
    uint8_t empty[] = {HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS, 1, 0};
    emit(empty, sizeof(empty));
    assert(totals.credit_events == 4 && totals.credit_entries == 5);
    uint32_t before = totals.credit_events;
    uint64_t packets_before = totals.credit_packets;
    emit(packet, sizeof(packet) - 1u); /* Truncated outer payload. */
    packet[2] = 4; /* Entry count exceeds payload; no partial accounting. */
    emit(packet, sizeof(packet));
    packet[2] = 2; /* Extra unclaimed entry also rejected. */
    emit(packet, sizeof(packet));
    packet[1] = 0;
    emit(packet, sizeof(packet)); /* Declared length excludes count, despite backing buffer. */
    uint8_t tiny[] = {HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS};
    emit(tiny, sizeof(tiny));
    emit(NULL, 0);
    uint8_t short_connection[] = {HCI_EVENT_CONNECTION_COMPLETE, 0};
    uint8_t short_gap[] = {HCI_EVENT_META_GAP, 1, GAP_SUBEVENT_LE_CONNECTION_COMPLETE};
    uint8_t short_update[] = {HCI_EVENT_LE_META, 1, HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE};
    uint8_t short_disconnect[] = {HCI_EVENT_DISCONNECTION_COMPLETE, 0};
    uint8_t short_role[] = {HCI_EVENT_ROLE_CHANGE, 0};
    emit(short_connection, sizeof(short_connection));
    emit(short_gap, sizeof(short_gap));
    emit(short_update, sizeof(short_update));
    emit(short_disconnect, sizeof(short_disconnect));
    emit(short_role, sizeof(short_role));
    assert(totals.malformed == 11);
    assert(totals.credit_events == before && totals.credit_packets == packets_before);
    registered->callback(HCI_ACL_DATA_PACKET, 0, tiny, sizeof(tiny));
    assert(totals.malformed == 11 && !print_calls);
    uint8_t many[255] = {HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS, 253, 63};
    for (unsigned i = 0; i < 63; ++i) {
        little_endian_store_16(many, 3 + 4 * i, 2);
        little_endian_store_16(many, 5 + 4 * i, 65535);
    }
    emit(many, sizeof(many));
    assert(totals.credit_events == before + 1 && totals.credit_entries == 68);
    assert(totals.credit_packets == packets_before + 63ULL * 65535u);
    for (unsigned length = 0; length < sizeof(many); ++length) emit(many, (uint16_t)length);
    assert(totals.credit_events == before + 1); /* Every truncated prefix is rejected. */
    many[2] = 255; /* Count multiplication must not wrap in 8 bits. */
    emit(many, sizeof(many));
    assert(totals.credit_events == before + 1);
}

static void test_long_input_gaps_and_rollover(void) {
    reset_test();
    microseconds = UINT32_MAX - 2000u;
    milliseconds = 100;
    hub_perf_input_report(0, true);
    assert(perf_inputs[0][1].gaps.count == 0);
    advance_ms(8);
    hub_perf_input_report(0, true);
    advance_ms(20);
    hub_perf_input_report(0, true);
    advance_ms(100);
    hub_perf_input_report(0, true);
    advance_ms(201);
    hub_perf_input_report(0, true);
    perf_input_t *metric = &perf_inputs[0][1];
    assert(metric->reports == 5 && metric->gaps.count == 4);
    assert(metric->gaps.minimum == 8000 && metric->gaps.maximum == 201000);
    assert(metric->gaps.sum == 329000 && metric->gaps.last == 201000);
    assert(metric->slow == 3 && metric->long_gaps == 1);
    assert(last_slow()->kind == PERF_INPUT_GAP && last_slow()->value == 201000);
    assert(last_slow()->handle == HCI_CON_HANDLE_INVALID && !last_slow()->generation);
    advance_ms(5000000); /* More than one microsecond wrap, measured coarsely. */
    hub_perf_input_report(0, true);
    assert(metric->gaps.last == 5000000000ULL && metric->coarse_gaps == 1);
    assert(metric->long_gaps == 2 && last_slow()->coarse);
    milliseconds = UINT32_MAX - 3u;
    microseconds = UINT32_MAX - 1000u;
    hub_perf_input_report(1, false);
    advance_ms(8);
    hub_perf_input_report(1, false);
    assert(perf_inputs[1][0].gaps.last == 8000 && !perf_inputs[1][0].coarse_gaps);
    uint32_t old_reports = metric->reports;
    hub_perf_input_report(HUB_INPUT_COUNT, true);
    assert(metric->reports == old_reports);
    connect_classic(1, 0, 1);
    advance_ms(500);
    hub_perf_input_report(0, true);
    assert(metric->reports == old_reports + 1 && metric->gaps.count == old_reports - 1);
    assert(!print_calls);
}

static void test_output_waits_and_history_preservation(void) {
    reset_test();
    connect_le(1, HCI_ROLE_SLAVE);
    hub_perf_parameter_request(1, true, 6, 6, 0, 200, 0);
    perf_event_t saved[2] = {control_history[0], control_history[1]};
    hub_perf_output_wait(1, 0, true, 100);
    hub_perf_output_wait(1, 0, true, 10000);
    hub_perf_output_wait(1, 0, true, 40000);
    perf_output_t *metric = &perf_outputs[0][1];
    assert(metric->waits.count == 3 && metric->waits.sum == 50100);
    assert(metric->waits.minimum == 100 && metric->waits.maximum == 40000);
    assert(metric->slow == 2 && slow_count == 1 && totals.slow_suppressed == 1);
    assert(perf_find_link(1)->output_slot == 0);
    assert(metric->generation == perf_find_link(1)->generation);
    for (unsigned i = 0; i < PERF_SLOW_HISTORY + 10u; ++i) {
        advance_ms(PERF_SAMPLE_MS);
        hub_perf_output_wait(1, 0, true, 12000);
    }
    assert(slow_count == PERF_SLOW_HISTORY && slow_evicted == 11);
    assert(control_count == 2 && !control_evicted);
    assert(memcmp(control_history, saved, sizeof(saved)) == 0);
    assert(metric->waits.count == PERF_SLOW_HISTORY + 13u);
    hub_perf_output_wait(0x55, 1, false, 15000);
    assert(perf_outputs[1][0].generation == 0 && last_slow()->handle == 0x55);
    hub_perf_output_wait(1, HUB_COMPUTER_COUNT, false, 99999);
    assert(!perf_outputs[0][0].waits.count);
    for (unsigned i = 0; i < PERF_CONTROL_HISTORY + 5u; ++i) hub_perf_parameter_timeout(1);
    assert(control_count == PERF_CONTROL_HISTORY && control_evicted == 7);
    assert(slow_count == PERF_SLOW_HISTORY && last_control()->kind == PERF_TIMEOUT);
    assert(!print_calls);
}

static void test_parameter_metadata(void) {
    reset_test();
    connect_le(7, HCI_ROLE_SLAVE);
    uint32_t generation = perf_find_link(7)->generation;
    hub_perf_parameter_request(7, true, 6, 12, 0, 222, -42);
    assert(totals.requests == 1 && last_control()->kind == PERF_REQUEST);
    assert(last_control()->generation == generation && last_control()->fast);
    assert(last_control()->minimum == 6 && last_control()->maximum == 12);
    assert(last_control()->latency == 0 && last_control()->timeout == 222);
    assert(last_control()->status == -42);
    assert(perf_find_link(7)->interval == 6 && perf_find_link(7)->timeout == 200);
    uint8_t update[] = {HCI_EVENT_LE_META, 10, HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE,
                        0x28, 7, 0, 24, 0, 4, 0, 200, 0};
    emit(update, sizeof(update));
    assert(perf_find_link(7)->interval == 6 && last_control()->status == 0x28);
    update[3] = 0;
    emit(update, sizeof(update));
    assert(perf_find_link(7)->interval == 24 && perf_find_link(7)->latency == 4);
    assert(last_control()->minimum == 24 && last_control()->kind == PERF_PARAMETERS);
    hub_perf_parameter_timeout(7);
    assert(totals.timeouts == 1 && last_control()->generation == generation);
    assert(perf_find_link(7)->interval == 24);
    hub_perf_parameter_request(0x55, false, 24, 24, 4, 200, 0);
    assert(!last_control()->generation && !last_control()->fast);
    assert(!print_calls);
}

static void test_remote_parameter_metadata(void) {
    reset_test();
    connect_le(7, HCI_ROLE_SLAVE);
    uint32_t generation = perf_find_link(7)->generation;
    uint8_t remote[] = {HCI_EVENT_LE_META, 11, HCI_SUBEVENT_LE_REMOTE_CONNECTION_PARAMETER_REQUEST,
                        7, 0, 12, 0, 24, 0, 4, 0, 222, 0};
    emit(remote, sizeof(remote));
    assert(totals.remote_hci == 1 && !totals.requests);
    assert(last_control()->kind == PERF_REMOTE_HCI && last_control()->generation == generation);
    assert(last_control()->minimum == 12 && last_control()->maximum == 24);
    assert(last_control()->latency == 4 && last_control()->timeout == 222);
    assert(perf_find_link(7)->interval == 6 && perf_find_link(7)->timeout == 200);
    uint8_t request[] = {L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_REQUEST, 10,
                         7, 0, 24, 0, 36, 0, 2, 0, 200, 0};
    l2cap_registered->callback(HCI_EVENT_PACKET, 0, request, sizeof(request));
    assert(totals.remote_l2cap == 1 && !totals.requests);
    assert(last_control()->kind == PERF_REMOTE_L2CAP && last_control()->generation == generation);
    assert(last_control()->minimum == 24 && last_control()->maximum == 36);
    assert(last_control()->latency == 2 && last_control()->timeout == 200);
    assert(perf_find_link(7)->interval == 6);
    uint8_t response[] = {L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_RESPONSE, 4, 7, 0, 1, 0};
    l2cap_registered->callback(HCI_EVENT_PACKET, 0, response, sizeof(response));
    assert(last_control()->kind == PERF_L2CAP_RESPONSE && last_control()->status == 1);
    response[4] = 0;
    l2cap_registered->callback(HCI_EVENT_PACKET, 0, response, sizeof(response));
    assert(totals.l2cap_responses == 2 && last_control()->status == 0);
    assert(perf_find_link(7)->interval == 6); /* Even acceptance is not an applied update. */
    request[2] = 0x55;
    l2cap_registered->callback(HCI_EVENT_PACKET, 0, request, sizeof(request));
    assert(last_control()->generation == 0 && last_control()->handle == 0x55);
    assert(last_control()->minimum == 24 && totals.remote_l2cap == 2);
    unsigned controls = control_count;
    emit(remote, sizeof(remote) - 1u);
    remote[1] = 10; /* Declared body too short despite sufficient backing storage. */
    emit(remote, sizeof(remote));
    l2cap_registered->callback(HCI_EVENT_PACKET, 0, request, sizeof(request) - 1u);
    request[1] = 9;
    l2cap_registered->callback(HCI_EVENT_PACKET, 0, request, sizeof(request));
    l2cap_registered->callback(HCI_EVENT_PACKET, 0, response, sizeof(response) - 1u);
    response[1] = 3;
    l2cap_registered->callback(HCI_EVENT_PACKET, 0, response, sizeof(response));
    l2cap_registered->callback(HCI_ACL_DATA_PACKET, 0, response, sizeof(response));
    assert(totals.malformed == 6 && control_count == controls);
    assert(totals.remote_hci == 1 && totals.remote_l2cap == 2 && totals.l2cap_responses == 2);
    assert(!print_calls);
    hub_perf_status();
    assert(strstr(printed, "peer-HCI-parameter-request"));
    assert(strstr(printed, "peer-L2CAP-parameter-request"));
    assert(strstr(printed, "peer-L2CAP-parameter-response"));
    assert(strstr(printed, "acceptance/application not implied"));
    assert(strstr(printed, "signaling response, not an applied update"));
    hub_perf_reset();
    assert(!totals.remote_hci && !totals.remote_l2cap && !totals.l2cap_responses);
    assert(perf_find_link(7)->generation == generation && perf_find_link(7)->interval == 6);
    assert(l2cap_registrations == 1);
}

static void test_reset_preserves_identity_without_samples_or_side_effects(void) {
    reset_test();
    connect_classic(1, 0, 1);
    connect_le(2, HCI_ROLE_SLAVE);
    complete(1, 5);
    advance_ms(50);
    complete(1, 6);
    hub_perf_input_report(0, true);
    hub_perf_output_wait(2, 3, true, 15000);
    hub_perf_parameter_request(2, true, 6, 6, 0, 200, 0);
    perf_timer.process(&perf_timer);
    uint32_t generation = perf_find_link(1)->generation;
    uint32_t generation2 = perf_find_link(2)->generation;
    uint32_t generation_counter = next_generation, deadline = timer_due;
    unsigned sets = timer_sets, adds = timer_adds;
    advance_ms(1);
    hub_perf_reset();
    assert(!control_count && !slow_count && !totals.requests && !totals.credit_packets);
    assert(!perf_inputs[0][1].clock.valid && !perf_inputs[0][1].gaps.count);
    assert(!perf_outputs[3][1].waits.count && !loop_lateness.count);
    assert(perf_find_link(1)->generation == generation);
    assert(perf_find_link(2)->generation == generation2 && perf_find_link(2)->output_slot == 3);
    assert(perf_find_link(2)->interval == 6 && perf_find_link(2)->timeout == 200);
    assert(!perf_find_link(1)->credits.packets && !perf_find_link(1)->credits.clock.valid);
    assert(next_generation == generation_counter && timer_due == deadline);
    assert(timer_sets == sets && timer_adds == adds && registrations == 1 && l2cap_registrations == 1);
    assert(reset_at == milliseconds && !print_calls);
    hub_perf_init();
    assert(timer_sets == sets && timer_adds == adds && registrations == 1 && l2cap_registrations == 1);
    hub_perf_status();
    assert(strstr(printed, "samples=0 timings=n/a"));
    assert(!strstr(printed, "min/avg/max/last="));
    assert(strstr(printed, "aggregate controller credits"));
    assert(strstr(printed, "including other ACL traffic"));
    assert(strstr(printed, "NOT per-mouse ACKs"));
    assert(strstr(printed, "host display time"));
    assert(strstr(printed, "RF retransmissions"));
    assert(strstr(printed, "long/ambiguous"));
    advance_ms(1);
    hub_perf_input_report(0, true);
    complete(1, 2);
    assert(!perf_inputs[0][1].gaps.count && !perf_find_link(1)->credits.gaps.count);
    assert(perf_find_link(1)->credits.packets == 2);
    disconnect_link(1, 0, 0x28);
    assert(last_control()->generation == generation && last_control()->value == 2);
}

static void test_scheduler_lateness_and_wrap(void) {
    reset_test();
    advance_ms(PERF_TIMER_MS + 1u);
    perf_timer.process(&perf_timer);
    assert(loop_lateness.count == 1 && loop_lateness.last == 0);
    advance_ms(PERF_TIMER_MS + 1u + 12u);
    perf_timer.process(&perf_timer);
    assert(loop_lateness.count == 2 && loop_lateness.last == 12);
    assert(totals.loop_slow == 1 && last_slow()->kind == PERF_LOOP && last_slow()->value == 12);
    milliseconds = UINT32_MAX - 10u;
    timer_due = milliseconds + PERF_TIMER_MS + 1u;
    hub_perf_reset();
    advance_ms(PERF_TIMER_MS + 1u + 7u);
    perf_timer.process(&perf_timer);
    assert(loop_lateness.count == 1 && loop_lateness.last == 7);
    assert(timer_due == milliseconds + PERF_TIMER_MS + 1u && totals.loop_slow == 1);
    uint64_t samples = loop_lateness.count;
    perf_timer.process(&perf_timer); /* Early callback must not become UINT32_MAX lateness. */
    assert(loop_lateness.count == samples);
    advance_ms(PERF_TIMER_MS + 1u + 6u);
    perf_timer.process(&perf_timer);
    assert(loop_lateness.count == 2 && loop_lateness.last == 6);
    assert(totals.loop_slow == 2 && totals.slow_suppressed == 1);
    assert(!print_calls);
}

static void test_credit_clock_wrap_and_status(void) {
    reset_test();
    connect_le(1, HCI_ROLE_SLAVE);
    microseconds = UINT32_MAX - 5u;
    milliseconds = UINT32_MAX - 10u;
    complete(1, 1);
    advance_ms(25);
    complete(1, 2);
    assert(perf_find_link(1)->credits.gaps.last == 25000);
    advance_ms(5000000);
    complete(1, 3);
    assert(perf_find_link(1)->credits.gaps.last == 5000000000ULL);
    assert(perf_find_link(1)->credits.coarse_gaps == 1 && last_slow()->coarse);
    hub_perf_input_report(0, true);
    advance_ms(301);
    hub_perf_input_report(0, true);
    hub_perf_output_wait(1, 0, true, 21000);
    hub_perf_parameter_request(1, true, 6, 12, 0, 200, 0);
    hub_perf_parameter_timeout(1);
    hub_perf_status();
    assert(strstr(printed, "long/ambiguous(>100ms)=1"));
    assert(strstr(printed, "min/avg/max/last=21000/21000/21000/21000us"));
    assert(strstr(printed, "inter-credit-gap"));
    assert(strstr(printed, "parameter-timeout"));
    assert(strstr(printed, "observed-PC=1"));
}

static void test_dump_boundaries_and_print_interference(void) {
    reset_test();
    connect_le(1, HCI_ROLE_SLAVE);
    milliseconds = UINT32_MAX - 10u;
    microseconds = UINT32_MAX - 10000u;
    timer_due = milliseconds + PERF_TIMER_MS + 1u;
    complete(1, 1);
    hub_perf_input_report(0, true);
    uint32_t start = milliseconds, start_us = microseconds, due = timer_due;
    unsigned sets = timer_sets, adds = timer_adds;
    print_delay_ms = 1;
    hub_perf_status();
    assert(dump_count == 1 && totals.dumps == 1);
    assert(dump_history[0].start_ms == start && dump_history[0].end_ms == milliseconds);
    assert(dump_history[0].duration_us == (uint32_t)(microseconds - start_us));
    assert(dump_history[0].duration_us == (uint64_t)print_calls * 1000u);
    assert(!dump_history[0].coarse && milliseconds < start); /* Millisecond wrap in the dump. */
    assert(strstr(printed, "dump-begin") && strstr(printed, "dump-body-end"));
    assert(strstr(printed, "retain printing stalls") && strstr(printed, "current span appears next time"));
    assert(!perf_inputs[0][1].gaps.count && !perf_find_link(1)->credits.gaps.count);
    assert(perf_inputs[0][1].clock.at_us == start_us && perf_find_link(1)->credits.clock.at_us == start_us);
    assert(timer_due == due && timer_sets == sets && timer_adds == adds);
    assert(!loop_lateness.count && control_count == 1);
    advance_ms(10);
    uint32_t elapsed = microseconds - start_us;
    hub_perf_input_report(0, true);
    complete(1, 2);
    hub_perf_output_wait(1, 0, true, elapsed);
    perf_timer.process(&perf_timer);
    assert(perf_inputs[0][1].gaps.last == elapsed);
    assert(perf_find_link(1)->credits.gaps.last == elapsed);
    assert(perf_outputs[0][1].waits.last == elapsed);
    assert(loop_lateness.last == (uint32_t)(milliseconds - due));
    print_delay_ms = 0;
    for (unsigned i = 0; i < PERF_DUMP_HISTORY + 1u; ++i) {
        advance_ms(1);
        printed_length = print_calls = 0;
        printed[0] = 0;
        hub_perf_status();
        assert(strstr(printed, "dump-span start="));
    }
    assert(dump_count == PERF_DUMP_HISTORY && totals.dumps == PERF_DUMP_HISTORY + 2u);
    assert(control_count == 1 && !control_evicted); /* Dumps cannot evict radio control history. */
    hub_perf_reset();
    assert(!dump_count && !dump_next && !totals.dumps);
    assert(!dump_history[0].duration_us && perf_find_link(1));
}

int main(void) {
    test_transport_lifecycle_and_reuse();
    test_classic_role_change_and_full_table();
    test_batched_completions_and_malformed_events();
    test_long_input_gaps_and_rollover();
    test_output_waits_and_history_preservation();
    test_parameter_metadata();
    test_remote_parameter_metadata();
    test_reset_preserves_identity_without_samples_or_side_effects();
    test_scheduler_lateness_and_wrap();
    test_credit_clock_wrap_and_status();
    test_dump_boundaries_and_print_interference();
    puts("hub_perf: all tests passed");
    return 0;
}
