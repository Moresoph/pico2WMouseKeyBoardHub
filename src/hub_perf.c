#include "hub_perf.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "btstack_config.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "gap.h"
#include "hci.h"
#include "l2cap.h"
#include "hub_config.h"
#include "hub_latency.h"

#define PERF_CONTROL_HISTORY 32u
#define PERF_SLOW_HISTORY 32u
#define PERF_DUMP_HISTORY 4u
#define PERF_TIMER_MS 25u
#define PERF_SAMPLE_MS 250u
#define PERF_INPUT_SLOW_US 20000u
#define PERF_INPUT_LONG_US 100000u
#define PERF_OUTPUT_SLOW_US 10000u
#define PERF_CREDIT_GAP_US 20000u
#define PERF_LOOP_SLOW_MS 5u
#define PERF_NO_SLOT 0xffu

enum {
    PERF_CONNECT, PERF_DISCONNECT, PERF_PARAMETERS, PERF_REQUEST, PERF_TIMEOUT,
    PERF_ROLE, PERF_INPUT_GAP, PERF_OUTPUT_WAIT, PERF_CREDITS, PERF_LOOP,
    PERF_REMOTE_HCI, PERF_REMOTE_L2CAP, PERF_L2CAP_RESPONSE
};
enum { PERF_TRANSPORT_UNKNOWN, PERF_CLASSIC, PERF_LE };

typedef struct {
    uint64_t count;
    uint64_t sum;
    uint64_t minimum;
    uint64_t maximum;
    uint64_t last;
} perf_samples_t;

typedef struct {
    bool valid;
    uint32_t at_ms;
} perf_gate_t;

typedef struct {
    bool valid;
    uint32_t at_us;
    uint32_t at_ms;
} perf_clock_t;

typedef struct {
    uint32_t events;
    uint64_t packets;
    uint16_t last_batch;
    uint32_t coarse_gaps;
    perf_clock_t clock;
    perf_samples_t gaps;
    perf_gate_t gate;
} perf_credits_t;

typedef struct {
    bool connected;
    uint16_t handle;
    uint8_t transport;
    uint8_t role;
    uint8_t output_slot;
    uint32_t generation;
    uint32_t connected_at;
    uint16_t interval;
    uint16_t latency;
    uint16_t timeout;
    perf_credits_t credits;
} perf_link_t;

typedef struct {
    uint32_t reports;
    uint32_t slow;
    uint32_t long_gaps;
    uint32_t coarse_gaps;
    perf_clock_t clock;
    perf_samples_t gaps;
    perf_gate_t gate;
} perf_input_t;

typedef struct {
    perf_samples_t waits;
    uint32_t slow;
    uint32_t at_ms;
    uint16_t handle;
    uint32_t generation;
    perf_gate_t gate;
} perf_output_t;

typedef struct {
    uint32_t at_ms;
    uint32_t generation;
    uint16_t handle;
    uint8_t kind;
    uint8_t transport;
    uint8_t role;
    uint8_t slot;
    bool mouse;
    bool fast;
    bool coarse;
    int status;
    uint16_t minimum;
    uint16_t maximum;
    uint16_t latency;
    uint16_t timeout;
    uint32_t count;
    uint64_t value;
} perf_event_t;

typedef struct {
    uint32_t start_ms;
    uint32_t end_ms;
    uint64_t duration_us;
    bool coarse;
} perf_dump_t;

typedef struct {
    uint32_t connects;
    uint32_t disconnects;
    uint32_t instant_passed;
    uint32_t updates;
    uint32_t requests;
    uint32_t timeouts;
    uint32_t remote_hci;
    uint32_t remote_l2cap;
    uint32_t l2cap_responses;
    uint32_t dumps;
    uint32_t malformed;
    uint32_t table_full;
    uint32_t input_boundaries;
    uint32_t credit_events;
    uint32_t credit_entries;
    uint64_t credit_packets;
    uint32_t unknown_entries;
    uint64_t unknown_packets;
    uint32_t slow_suppressed;
    uint32_t loop_slow;
} perf_totals_t;

static perf_link_t perf_links[MAX_NR_HCI_CONNECTIONS];
static perf_input_t perf_inputs[HUB_INPUT_COUNT][2];
static perf_output_t perf_outputs[HUB_COMPUTER_COUNT][2];
static perf_event_t control_history[PERF_CONTROL_HISTORY];
static perf_event_t slow_history[PERF_SLOW_HISTORY];
static perf_dump_t dump_history[PERF_DUMP_HISTORY];
static unsigned control_next, control_count, slow_next, slow_count;
static unsigned dump_next, dump_count;
static uint32_t control_evicted, slow_evicted;
static uint32_t next_generation;
static uint32_t reset_at, timer_due;
static perf_totals_t totals;
static perf_samples_t loop_lateness;
static perf_gate_t loop_gate, unknown_gate;
static btstack_packet_callback_registration_t perf_registration;
static btstack_packet_callback_registration_t perf_l2cap_registration;
static btstack_timer_source_t perf_timer;
static bool perf_initialized;

static perf_link_t *perf_find_link(uint16_t handle) {
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        if (perf_links[i].connected && perf_links[i].handle == handle) return &perf_links[i];
    }
    return NULL;
}

static void perf_sample(perf_samples_t *samples, uint64_t value) {
    if (!samples->count || value < samples->minimum) samples->minimum = value;
    if (!samples->count || value > samples->maximum) samples->maximum = value;
    ++samples->count;
    samples->sum += value;
    samples->last = value;
}

static uint64_t perf_elapsed(const perf_clock_t *clock, uint32_t now_us,
                             uint32_t now_ms, bool *coarse) {
    uint32_t elapsed_ms = now_ms - clock->at_ms;
    /* A long silence may span multiple 32-bit microsecond wraps. Do not alias it
       into a short report gap. Millisecond resolution is enough for that case. */
    *coarse = elapsed_ms >= UINT32_MAX / 1000u;
    return *coarse ? (uint64_t)elapsed_ms * 1000u : (uint32_t)(now_us - clock->at_us);
}

static void perf_clock_set(perf_clock_t *clock, uint32_t now_us, uint32_t now_ms) {
    clock->valid = true;
    clock->at_us = now_us;
    clock->at_ms = now_ms;
}

static perf_event_t perf_event(unsigned kind, uint16_t handle, uint32_t now) {
    perf_event_t event;
    memset(&event, 0, sizeof(event));
    event.kind = (uint8_t)kind;
    event.handle = handle;
    event.at_ms = now;
    event.role = HCI_ROLE_INVALID;
    event.slot = PERF_NO_SLOT;
    perf_link_t *link = perf_find_link(handle);
    if (link) {
        event.generation = link->generation;
        event.transport = link->transport;
        event.role = link->role;
        event.slot = link->output_slot;
    }
    return event;
}

static void perf_control_push(perf_event_t event) {
    control_history[control_next] = event;
    control_next = (control_next + 1u) % PERF_CONTROL_HISTORY;
    if (control_count < PERF_CONTROL_HISTORY) ++control_count;
    else ++control_evicted;
}

static void perf_slow_push(perf_event_t event, perf_gate_t *gate) {
    if (gate->valid && (uint32_t)(event.at_ms - gate->at_ms) < PERF_SAMPLE_MS) {
        ++totals.slow_suppressed;
        return;
    }
    gate->valid = true;
    gate->at_ms = event.at_ms;
    slow_history[slow_next] = event;
    slow_next = (slow_next + 1u) % PERF_SLOW_HISTORY;
    if (slow_count < PERF_SLOW_HISTORY) ++slow_count;
    else ++slow_evicted;
}

static void perf_input_boundary(void) {
    /* The input hook identifies a slot, not a handle. Invalidate both baselines
       at an input-side ACL boundary rather than inventing a slot/link mapping. */
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        for (unsigned j = 0; j < 2; ++j) perf_inputs[i][j].clock.valid = false;
    }
    ++totals.input_boundaries;
}

static void perf_connect(uint16_t handle, uint8_t transport, uint8_t role,
                          uint16_t interval, uint16_t latency, uint16_t timeout) {
    perf_link_t *link = perf_find_link(handle);
    if (!link) {
        for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
            if (!perf_links[i].connected) { link = &perf_links[i]; break; }
        }
    }
    if (!link) {
        ++totals.table_full;
        return;
    }
    memset(link, 0, sizeof(*link));
    link->connected = true;
    link->handle = handle;
    link->transport = transport;
    link->role = role;
    link->output_slot = PERF_NO_SLOT;
    if (++next_generation == 0) ++next_generation;
    link->generation = next_generation;
    link->connected_at = btstack_run_loop_get_time_ms();
    link->interval = interval;
    link->latency = latency;
    link->timeout = timeout;
    ++totals.connects;
    if (transport == PERF_CLASSIC || role == HCI_ROLE_MASTER) perf_input_boundary();
    perf_event_t event = perf_event(PERF_CONNECT, handle, link->connected_at);
    event.minimum = event.maximum = interval;
    event.latency = latency;
    event.timeout = timeout;
    perf_control_push(event);
}

void hub_perf_input_report(unsigned input, bool mouse) {
    if (input >= HUB_INPUT_COUNT) return;
    perf_input_t *metric = &perf_inputs[input][mouse ? 1 : 0];
    uint32_t now_us = hub_latency_now_us();
    uint32_t now_ms = btstack_run_loop_get_time_ms();
    if (metric->clock.valid) {
        bool coarse;
        uint64_t gap = perf_elapsed(&metric->clock, now_us, now_ms, &coarse);
        perf_sample(&metric->gaps, gap);
        if (coarse) ++metric->coarse_gaps;
        if (gap > PERF_INPUT_LONG_US) ++metric->long_gaps;
        if (gap >= PERF_INPUT_SLOW_US) {
            ++metric->slow;
            perf_event_t event = perf_event(PERF_INPUT_GAP, HCI_CON_HANDLE_INVALID, now_ms);
            event.slot = (uint8_t)input;
            event.mouse = mouse;
            event.value = gap;
            event.coarse = coarse;
            perf_slow_push(event, &metric->gate);
        }
    }
    ++metric->reports;
    perf_clock_set(&metric->clock, now_us, now_ms);
}

void hub_perf_output_wait(uint16_t handle, unsigned slot, bool mouse, uint32_t wait_us) {
    if (slot >= HUB_COMPUTER_COUNT) return;
    perf_output_t *metric = &perf_outputs[slot][mouse ? 1 : 0];
    perf_link_t *link = perf_find_link(handle);
    if (link) link->output_slot = (uint8_t)slot;
    perf_sample(&metric->waits, wait_us);
    metric->at_ms = btstack_run_loop_get_time_ms();
    metric->handle = handle;
    metric->generation = link ? link->generation : 0;
    if (wait_us >= PERF_OUTPUT_SLOW_US) {
        ++metric->slow;
        perf_event_t event = perf_event(PERF_OUTPUT_WAIT, handle, metric->at_ms);
        event.slot = (uint8_t)slot;
        event.mouse = mouse;
        event.value = wait_us;
        perf_slow_push(event, &metric->gate);
    }
}

void hub_perf_parameter_request(uint16_t handle, bool fast, uint16_t min_interval,
                                uint16_t max_interval, uint16_t peripheral_latency,
                                uint16_t timeout, int status) {
    perf_event_t event = perf_event(PERF_REQUEST, handle, btstack_run_loop_get_time_ms());
    event.fast = fast;
    event.minimum = min_interval;
    event.maximum = max_interval;
    event.latency = peripheral_latency;
    event.timeout = timeout;
    event.status = status;
    ++totals.requests;
    perf_control_push(event);
}

void hub_perf_parameter_timeout(uint16_t handle) {
    ++totals.timeouts;
    perf_control_push(perf_event(PERF_TIMEOUT, handle, btstack_run_loop_get_time_ms()));
}

static void perf_completed(const uint8_t *packet, uint16_t size) {
    if (size < 3 || size != 3u + 4u * packet[2]) {
        ++totals.malformed;
        return;
    }
    uint32_t now_us = hub_latency_now_us();
    uint32_t now_ms = btstack_run_loop_get_time_ms();
    ++totals.credit_events;
    for (unsigned i = 0; i < packet[2]; ++i) {
        uint16_t handle = little_endian_read_16(packet, 3 + 4 * i);
        uint16_t count = little_endian_read_16(packet, 5 + 4 * i);
        ++totals.credit_entries;
        totals.credit_packets += count;
        perf_link_t *link = perf_find_link(handle);
        if (!link) {
            ++totals.unknown_entries;
            totals.unknown_packets += count;
            perf_event_t event = perf_event(PERF_CREDITS, handle, now_ms);
            event.count = count;
            perf_slow_push(event, &unknown_gate);
            continue;
        }
        perf_credits_t *credits = &link->credits;
        ++credits->events;
        credits->packets += count;
        credits->last_batch = count;
        if (credits->clock.valid) {
            bool coarse;
            uint64_t gap = perf_elapsed(&credits->clock, now_us, now_ms, &coarse);
            perf_sample(&credits->gaps, gap);
            if (coarse) ++credits->coarse_gaps;
            if (gap >= PERF_CREDIT_GAP_US) {
                perf_event_t event = perf_event(PERF_CREDITS, handle, now_ms);
                event.value = gap;
                event.count = count;
                event.coarse = coarse;
                perf_slow_push(event, &credits->gate);
            }
        }
        perf_clock_set(&credits->clock, now_us, now_ms);
    }
}

static void perf_peer_request(unsigned kind, const uint8_t *packet, unsigned offset) {
    perf_event_t event = perf_event(kind, little_endian_read_16(packet, offset),
                                    btstack_run_loop_get_time_ms());
    event.minimum = little_endian_read_16(packet, offset + 2);
    event.maximum = little_endian_read_16(packet, offset + 4);
    event.latency = little_endian_read_16(packet, offset + 6);
    event.timeout = little_endian_read_16(packet, offset + 8);
    if (kind == PERF_REMOTE_HCI) ++totals.remote_hci;
    else ++totals.remote_l2cap;
    perf_control_push(event);
}

static void perf_hci_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet,
                              uint16_t size) {
    (void)channel;
    if (packet_type != HCI_EVENT_PACKET) return;
    if (size < 2 || (unsigned)packet[1] + 2u > size) {
        ++totals.malformed;
        return;
    }
    size = (uint16_t)packet[1] + 2u;
    switch (packet[0]) {
        case HCI_EVENT_CONNECTION_COMPLETE:
            if (size < 13) { ++totals.malformed; break; }
            if (hci_event_connection_complete_get_status(packet) != 0
                || hci_event_connection_complete_get_link_type(packet) != 1) break;
            {
                uint16_t handle = hci_event_connection_complete_get_connection_handle(packet);
                perf_connect(handle, PERF_CLASSIC, gap_get_role(handle), 0, 0, 0);
            }
            break;
        case HCI_EVENT_META_GAP:
            if (size < 3) { ++totals.malformed; break; }
            if (packet[2] != GAP_SUBEVENT_LE_CONNECTION_COMPLETE) break;
            if (size < 33) { ++totals.malformed; break; }
            if (gap_subevent_le_connection_complete_get_status(packet) != 0) break;
            perf_connect(gap_subevent_le_connection_complete_get_connection_handle(packet),
                         PERF_LE, gap_subevent_le_connection_complete_get_role(packet),
                         gap_subevent_le_connection_complete_get_conn_interval(packet),
                         gap_subevent_le_connection_complete_get_conn_latency(packet),
                         gap_subevent_le_connection_complete_get_supervision_timeout(packet));
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE: {
            if (size < 6) { ++totals.malformed; break; }
            uint16_t handle = hci_event_disconnection_complete_get_connection_handle(packet);
            perf_link_t *link = perf_find_link(handle);
            perf_event_t event = perf_event(PERF_DISCONNECT, handle, btstack_run_loop_get_time_ms());
            event.status = hci_event_disconnection_complete_get_status(packet);
            event.count = hci_event_disconnection_complete_get_reason(packet);
            if (link) event.value = link->credits.packets;
            perf_control_push(event);
            if (event.status != 0) break;
            ++totals.disconnects;
            if (event.count == 0x28) ++totals.instant_passed;
            if (link) {
                if (link->transport == PERF_CLASSIC || link->role == HCI_ROLE_MASTER) perf_input_boundary();
                memset(link, 0, sizeof(*link));
            }
            break;
        }
        case HCI_EVENT_LE_META: {
            if (size < 3) { ++totals.malformed; break; }
            if (packet[2] == HCI_SUBEVENT_LE_REMOTE_CONNECTION_PARAMETER_REQUEST) {
                if (size < 13) { ++totals.malformed; break; }
                perf_peer_request(PERF_REMOTE_HCI, packet, 3);
                break;
            }
            if (packet[2] != HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE) break;
            if (size < 12) { ++totals.malformed; break; }
            uint16_t handle = hci_subevent_le_connection_update_complete_get_connection_handle(packet);
            perf_event_t event = perf_event(PERF_PARAMETERS, handle, btstack_run_loop_get_time_ms());
            event.status = hci_subevent_le_connection_update_complete_get_status(packet);
            event.minimum = event.maximum = hci_subevent_le_connection_update_complete_get_conn_interval(packet);
            event.latency = hci_subevent_le_connection_update_complete_get_conn_latency(packet);
            event.timeout = hci_subevent_le_connection_update_complete_get_supervision_timeout(packet);
            ++totals.updates;
            perf_control_push(event);
            perf_link_t *link = perf_find_link(handle);
            if (link && event.status == 0) {
                link->interval = event.minimum;
                link->latency = event.latency;
                link->timeout = event.timeout;
            }
            break;
        }
        case L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_REQUEST:
            if (size < 12) { ++totals.malformed; break; }
            perf_peer_request(PERF_REMOTE_L2CAP, packet, 2);
            break;
        case L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_RESPONSE: {
            if (size < 6) { ++totals.malformed; break; }
            perf_event_t event = perf_event(PERF_L2CAP_RESPONSE,
                l2cap_event_connection_parameter_update_response_get_handle(packet),
                btstack_run_loop_get_time_ms());
            event.status = l2cap_event_connection_parameter_update_response_get_result(packet);
            ++totals.l2cap_responses;
            perf_control_push(event);
            break;
        }
        case HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS:
            perf_completed(packet, size);
            break;
        case HCI_EVENT_ROLE_CHANGE:
            if (size < 10) { ++totals.malformed; break; }
            if (packet[2]) break;
            /* Role-change events carry an address, not a handle. Query live HCI
               roles instead of keeping any peer addresses in this recorder. */
            for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
                perf_link_t *link = &perf_links[i];
                if (!link->connected || link->transport != PERF_CLASSIC) continue;
                uint8_t role = gap_get_role(link->handle);
                if (role == HCI_ROLE_INVALID || role == link->role) continue;
                link->role = role;
                perf_control_push(perf_event(PERF_ROLE, link->handle, btstack_run_loop_get_time_ms()));
            }
            break;
        default:
            break;
    }
}

static void perf_service(btstack_timer_source_t *source) {
    uint32_t now = btstack_run_loop_get_time_ms();
    if ((int32_t)(now - timer_due) >= 0) {
        uint32_t late = now - timer_due;
        perf_sample(&loop_lateness, late);
        if (late >= PERF_LOOP_SLOW_MS) {
            ++totals.loop_slow;
            perf_event_t event = perf_event(PERF_LOOP, HCI_CON_HANDLE_INVALID, now);
            event.value = late;
            perf_slow_push(event, &loop_gate);
        }
    }
    btstack_run_loop_set_timer(source, PERF_TIMER_MS);
    /* Pico's run loop rounds its timer deadline up by a millisecond. */
    timer_due = (uint32_t)source->timeout;
    btstack_run_loop_add_timer(source);
}

void hub_perf_reset(void) {
    memset(&totals, 0, sizeof(totals));
    memset(perf_inputs, 0, sizeof(perf_inputs));
    memset(perf_outputs, 0, sizeof(perf_outputs));
    memset(control_history, 0, sizeof(control_history));
    memset(slow_history, 0, sizeof(slow_history));
    memset(dump_history, 0, sizeof(dump_history));
    memset(&loop_lateness, 0, sizeof(loop_lateness));
    memset(&loop_gate, 0, sizeof(loop_gate));
    memset(&unknown_gate, 0, sizeof(unknown_gate));
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        memset(&perf_links[i].credits, 0, sizeof(perf_links[i].credits));
    }
    control_next = control_count = slow_next = slow_count = 0;
    dump_next = dump_count = 0;
    control_evicted = slow_evicted = 0;
    reset_at = btstack_run_loop_get_time_ms();
}

void hub_perf_init(void) {
    if (perf_initialized) return;
    perf_initialized = true;
    hub_perf_reset();
    perf_registration.callback = perf_hci_handler;
    hci_add_event_handler(&perf_registration);
    perf_l2cap_registration.callback = perf_hci_handler;
    l2cap_add_event_handler(&perf_l2cap_registration);
    btstack_run_loop_set_timer_handler(&perf_timer, perf_service);
    btstack_run_loop_set_timer(&perf_timer, PERF_TIMER_MS);
    timer_due = (uint32_t)perf_timer.timeout;
    btstack_run_loop_add_timer(&perf_timer);
}

static const char *perf_transport_name(unsigned transport) {
    return transport == PERF_LE ? "LE" : transport == PERF_CLASSIC ? "Classic" : "unknown";
}

static const char *perf_role_name(unsigned role) {
    return role == HCI_ROLE_MASTER ? "central" : role == HCI_ROLE_SLAVE ? "peripheral" : "unknown";
}

static void perf_print_samples(const perf_samples_t *samples, const char *unit) {
    if (!samples->count) {
        printf("samples=0 timings=n/a");
        return;
    }
    printf("samples=%" PRIu64 " min/avg/max/last=%" PRIu64 "/%" PRIu64 "/%" PRIu64 "/%" PRIu64 "%s",
           samples->count, samples->minimum, samples->sum / samples->count,
           samples->maximum, samples->last, unit);
}

static void perf_print_event(const perf_event_t *event) {
    static const char *names[] = {"connect", "disconnect", "applied-update", "parameter-request",
                                  "parameter-timeout", "role", "input-gap", "output-wait",
                                  "controller-credits", "loop-late", "peer-HCI-parameter-request",
                                  "peer-L2CAP-parameter-request", "peer-L2CAP-parameter-response"};
    printf("[trace] t=%" PRIu32 "ms %s", event->at_ms, names[event->kind]);
    if (event->handle != HCI_CON_HANDLE_INVALID) {
        printf(" handle=0x%04x gen=%" PRIu32 " %s/%s",
               event->handle, event->generation, perf_transport_name(event->transport),
               perf_role_name(event->role));
    }
    if (event->kind == PERF_INPUT_GAP || event->kind == PERF_OUTPUT_WAIT) {
        printf(" %s=%u %s value=%" PRIu64 "us%s",
               event->kind == PERF_INPUT_GAP ? "input" : "PC", event->slot + 1u,
               event->mouse ? "mouse" : "keyboard", event->value,
               event->coarse ? " (coarse ms clock)" : "");
        if (event->kind == PERF_INPUT_GAP && event->value > PERF_INPUT_LONG_US) {
            printf(" long/ambiguous");
        }
    } else if (event->kind == PERF_CONNECT || event->kind == PERF_PARAMETERS
               || event->kind == PERF_REQUEST || event->kind == PERF_REMOTE_HCI
               || event->kind == PERF_REMOTE_L2CAP) {
        bool peer = event->kind == PERF_REMOTE_HCI || event->kind == PERF_REMOTE_L2CAP;
        if (peer) printf(" observed; acceptance/application not implied");
        else printf(" status=%d", event->status);
        if (event->kind == PERF_REQUEST) printf(" fast=%u", event->fast);
        if (event->transport == PERF_LE || event->kind == PERF_PARAMETERS
            || event->kind == PERF_REQUEST || peer) {
            printf(" interval=%u..%u(1.25ms) latency=%u timeout=%u(10ms)",
                   event->minimum, event->maximum, event->latency, event->timeout);
            if (event->kind == PERF_PARAMETERS && event->status) printf(" NOT applied");
        }
    } else if (event->kind == PERF_L2CAP_RESPONSE) {
        printf(" result=0x%04x (signaling response, not an applied update)", event->status);
    } else if (event->kind == PERF_DISCONNECT) {
        printf(" status=%d reason=0x%02" PRIx32 " credits=%" PRIu64,
               event->status, event->count, event->value);
    } else if (event->kind == PERF_CREDITS) {
        printf(" batch=%" PRIu32, event->count);
        if (event->generation) {
            printf(" inter-credit-gap=%" PRIu64 "us%s (traffic-dependent)",
                   event->value, event->coarse ? " coarse" : "");
        } else {
            printf(" unmatched handle; generation unknown");
        }
    } else if (event->kind == PERF_LOOP) {
        printf(" lateness=%" PRIu64 "ms", event->value);
    }
    printf("\n");
}

void hub_perf_status(void) {
    uint32_t now = btstack_run_loop_get_time_ms();
    perf_clock_t started;
    perf_clock_set(&started, hub_latency_now_us(), now);
    printf("[trace] dump-begin t=%" PRIu32 "ms; synchronous printing perturbs timing\n", now);
    printf("[trace] RAM metadata only; since-reset=%" PRIu32 "ms; timestamps modulo 2^32 ms\n", now - reset_at);
    printf("[trace] HCI completed-packet counts are aggregate controller credits, including other ACL traffic.\n");
    printf("[trace] NOT per-mouse ACKs, end-to-end latency, host display time, RF retransmissions, or per-report controller residence.\n");
    printf("[trace] credit generations follow observed handle lifetimes; HCI credits alone cannot identify a report or a stale credit after handle reuse.\n");
    printf("[trace] topology: LE central=input-side, LE peripheral=PC-side, Classic ACL=input-side; observed-PC is learned from timed submissions.\n");
    printf("[trace] input gaps include >100ms long/ambiguous gaps (idle OR delay); no activity inference. Input slots are not mapped to handles.\n");
    printf("[trace] output waits cover timed ordinary events through successful ATT submission only; slot summaries span generations.\n");
    printf("[trace] input/credit spans >=~71min use coarse ms clock; durations assume <one 32-bit ms wrap (~49d).\n");
    printf("[trace] input/credit gaps, output waits and loop lateness retain printing stalls; nothing is subtracted or rebaselined.\n");
    printf("[trace] completed-dumps=%" PRIu32 " retained=%u/%u; full spans include the closing line; current span appears next time\n",
           totals.dumps, dump_count, PERF_DUMP_HISTORY);
    for (unsigned i = 0; i < dump_count; ++i) {
        const perf_dump_t *dump = &dump_history[(dump_next + PERF_DUMP_HISTORY - dump_count + i) % PERF_DUMP_HISTORY];
        printf("[trace] dump-span start=%" PRIu32 "ms end=%" PRIu32 "ms duration=%" PRIu64 "us%s\n",
               dump->start_ms, dump->end_ms, dump->duration_us, dump->coarse ? " (coarse ms clock)" : "");
    }
    printf("[trace] connects=%" PRIu32 " disconnects=%" PRIu32 " instant-passed(0x28)=%" PRIu32
           " updates=%" PRIu32 " requests=%" PRIu32 " timeouts=%" PRIu32
           " malformed=%" PRIu32 " table-full=%" PRIu32 " input-baseline-boundaries=%" PRIu32 "\n",
           totals.connects, totals.disconnects, totals.instant_passed, totals.updates,
           totals.requests, totals.timeouts, totals.malformed, totals.table_full, totals.input_boundaries);
    printf("[trace] observed-peer-HCI-requests=%" PRIu32 " peer-L2CAP-requests=%" PRIu32
           " peer-L2CAP-responses=%" PRIu32 " (requests/responses do not establish actual parameters)\n",
           totals.remote_hci, totals.remote_l2cap, totals.l2cap_responses);
    printf("[trace] controller-credit events=%" PRIu32 " entries=%" PRIu32 " packets=%" PRIu64
           " unmatched-entries=%" PRIu32 " unmatched-packets=%" PRIu64 "\n",
           totals.credit_events, totals.credit_entries, totals.credit_packets,
           totals.unknown_entries, totals.unknown_packets);
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        const perf_link_t *link = &perf_links[i];
        if (!link->connected) continue;
        printf("[trace] link=0x%04x gen=%" PRIu32 " %s/%s age=%" PRIu32 "ms",
               link->handle, link->generation, perf_transport_name(link->transport),
               perf_role_name(link->role), now - link->connected_at);
        if (link->output_slot != PERF_NO_SLOT) printf(" observed-PC=%u", link->output_slot + 1u);
        if (link->transport == PERF_LE) {
            printf(" actual-interval=%u(1.25ms) latency=%u timeout=%u(10ms)",
                   link->interval, link->latency, link->timeout);
        }
        printf(" credit-entries=%" PRIu32 " packets=%" PRIu64,
               link->credits.events, link->credits.packets);
        if (link->credits.clock.valid) {
            printf(" last-batch=%u age=%" PRIu32 "ms", link->credits.last_batch, now - link->credits.clock.at_ms);
        }
        printf(" inter-credit-gap ");
        perf_print_samples(&link->credits.gaps, "us");
        printf(" coarse=%" PRIu32 "\n", link->credits.coarse_gaps);
    }
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        for (unsigned j = 0; j < 2; ++j) {
            const perf_input_t *metric = &perf_inputs[i][j];
            printf("[trace] input=%u %s reports=%" PRIu32 " slow(>=20ms)=%" PRIu32
                   " long/ambiguous(>100ms)=%" PRIu32 " coarse=%" PRIu32 " gap ",
                   i + 1, j ? "mouse" : "keyboard", metric->reports, metric->slow,
                   metric->long_gaps, metric->coarse_gaps);
            perf_print_samples(&metric->gaps, "us");
            if (metric->clock.valid) printf(" last-report-age=%" PRIu32 "ms", now - metric->clock.at_ms);
            else printf(" report-baseline=n/a");
            printf("\n");
        }
    }
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) {
        for (unsigned j = 0; j < 2; ++j) {
            const perf_output_t *metric = &perf_outputs[i][j];
            printf("[trace] PC=%u %s slow(>=10ms)=%" PRIu32 " queue-to-ATT ",
                   i + 1, j ? "mouse" : "keyboard", metric->slow);
            perf_print_samples(&metric->waits, "us");
            if (metric->waits.count) {
                printf(" last-handle=0x%04x gen=%" PRIu32 " age=%" PRIu32 "ms",
                       metric->handle, metric->generation, now - metric->at_ms);
            }
            printf("\n");
        }
    }
    printf("[trace] run-loop probe requested-period=%ums late(>=5ms)=%" PRIu32 " ", PERF_TIMER_MS, totals.loop_slow);
    perf_print_samples(&loop_lateness, "ms");
    printf(" (includes time spent printing status)\n");
    printf("[trace] control-history=%u/%u evicted=%" PRIu32
           " slow-history=%u/%u evicted=%" PRIu32 " throttled=%" PRIu32
           " (slow samples limited to one/source/%ums; counters include all)\n",
           control_count, PERF_CONTROL_HISTORY, control_evicted, slow_count,
           PERF_SLOW_HISTORY, slow_evicted, totals.slow_suppressed, PERF_SAMPLE_MS);
    for (unsigned i = 0; i < control_count; ++i) {
        perf_print_event(&control_history[(control_next + PERF_CONTROL_HISTORY - control_count + i) % PERF_CONTROL_HISTORY]);
    }
    for (unsigned i = 0; i < slow_count; ++i) {
        perf_print_event(&slow_history[(slow_next + PERF_SLOW_HISTORY - slow_count + i) % PERF_SLOW_HISTORY]);
    }
    printf("[trace] dump-body-end t=%" PRIu32 "ms; full return-time boundary retained in RAM\n",
           btstack_run_loop_get_time_ms());
    perf_dump_t *dump = &dump_history[dump_next];
    dump->start_ms = started.at_ms;
    dump->end_ms = btstack_run_loop_get_time_ms();
    dump->duration_us = perf_elapsed(&started, hub_latency_now_us(), dump->end_ms, &dump->coarse);
    dump_next = (dump_next + 1u) % PERF_DUMP_HISTORY;
    if (dump_count < PERF_DUMP_HISTORY) ++dump_count;
    ++totals.dumps;
}
