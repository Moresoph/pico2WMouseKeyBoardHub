#include "hub_latency.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "btstack_config.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "btstack_tlv.h"
#include "gap.h"
#include "hci.h"
#include "l2cap.h"
#include "hub_config.h"
#include "hub_input.h"
#include "hub_output.h"
#include "hub_perf.h"
#include "hub_security.h"

#define LATENCY_FAST_INTERVAL_MIN 6u
#define LATENCY_FAST_INTERVAL_MAX 12u
#define LATENCY_REQUEST_TIMEOUT_MS 5000u
#define LATENCY_SETTLE_MS 2000u
#define LATENCY_SETTINGS_TAG 0x50484c31u /* PHL1 */

typedef struct {
    bool connected;
    bool attempted;
    bool requested_fast;
    bool pending;
    bool remote_pending;
    bool tuned;
    uint8_t role;
    uint16_t handle;
    uint16_t interval;
    uint16_t latency;
    uint16_t timeout;
    uint16_t baseline_interval;
    uint16_t baseline_latency;
    uint16_t baseline_timeout;
    uint32_t connected_at;
    uint32_t request_at;
    uint32_t remote_request_at;
    unsigned requests;
    unsigned satisfied;
} latency_link_t;

typedef struct {
    uint32_t count;
    uint32_t last;
    uint32_t samples;
    uint32_t minimum;
    uint32_t maximum;
    uint64_t sum;
    uint32_t idle_gaps;
} input_timing_t;

static latency_link_t links[MAX_NR_HCI_CONNECTIONS];
static input_timing_t timing[HUB_INPUT_COUNT][2];
static btstack_packet_callback_registration_t registration;
static btstack_packet_callback_registration_t l2cap_registration;
static btstack_timer_source_t timer;
static bool fast_mode;
static bool working;
static uint32_t radio_activity_at;
static uint16_t observed_active;

static bool save_mode(bool enabled) {
    const btstack_tlv_t *tlv;
    void *context;
    btstack_tlv_get_instance(&tlv, &context);
    if (!tlv) {
        printf("[latency] settings storage unavailable; mode unchanged\n");
        return false;
    }
    const uint8_t record[8] = {'L', 'A', 'T', 1, enabled ? 1 : 0,
                               enabled ? 0xfe : 0xff, 0, 0};
    uint8_t readback[8];
    if (tlv->store_tag(context, LATENCY_SETTINGS_TAG, record, sizeof(record)) != 0
        || tlv->get_tag(context, LATENCY_SETTINGS_TAG, readback, sizeof(readback)) != sizeof(readback)
        || memcmp(record, readback, sizeof(record)) != 0) {
        printf("[latency] mode persistence failed; current mode retained, reboot setting uncertain\n");
        return false;
    }
    return true;
}

static void load_mode(void) {
    const btstack_tlv_t *tlv;
    void *context;
    btstack_tlv_get_instance(&tlv, &context);
    if (!tlv) {
        printf("[latency] settings storage unavailable; using normal mode\n");
        return;
    }
    uint8_t record[9];
    int size = tlv->get_tag(context, LATENCY_SETTINGS_TAG, record, sizeof(record));
    if (size == 0) return;
    if (size != 8 || memcmp(record, "LAT", 3) || record[3] != 1 || record[4] > 1
        || record[5] != (uint8_t)(record[4] ^ 0xffu) || record[6] || record[7]) {
        printf("[latency] corrupt/unsupported setting; using normal mode\n");
        return;
    }
    fast_mode = record[4] != 0;
    hub_output_set_low_latency(fast_mode);
    printf("[latency] restored mode=%s\n", fast_mode ? "fast" : "normal");
}

static latency_link_t *find_link(uint16_t handle) {
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        if (links[i].connected && links[i].handle == handle) return &links[i];
    }
    return NULL;
}

void hub_latency_input_report(unsigned input, bool mouse) {
    if (input >= HUB_INPUT_COUNT) {
        printf("[latency] invalid input timing source\n");
        return;
    }
    input_timing_t *metric = &timing[input][mouse ? 1 : 0];
    hub_perf_input_report(input, mouse);
    uint32_t now = hub_latency_now_us();
    if (metric->count) {
        uint32_t gap = now - metric->last;
        if (gap > 100000u) {
            ++metric->idle_gaps;
        } else {
            if (!metric->samples || gap < metric->minimum) metric->minimum = gap;
            if (gap > metric->maximum) metric->maximum = gap;
            ++metric->samples;
            metric->sum += gap;
        }
    }
    ++metric->count;
    metric->last = now;
}

static void print_link(const latency_link_t *link) {
    uint32_t interval_us = (uint32_t)link->interval * 1250u;
    printf("[latency] LE handle=0x%04x role=%s interval=%" PRIu32 ".%03" PRIu32
           "ms latency=%u timeout=%ums fast-requested=%u pending=%u remote-pending=%u requests=%u satisfied=%u\n",
           link->handle, link->role == HCI_ROLE_MASTER ? "input-central" : "PC-peripheral",
           interval_us / 1000u, interval_us % 1000u, link->latency,
           link->timeout * 10u, link->requested_fast, link->pending, link->remote_pending,
           link->requests, link->satisfied);
}

static void event_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type != HCI_EVENT_PACKET || size < 2) return;
    if ((unsigned)packet[1] + 2u > size) return;
    size = (uint16_t)packet[1] + 2u;
    if (packet[0] == BTSTACK_EVENT_STATE && size >= 3) {
        working = btstack_event_state_get_state(packet) == HCI_STATE_WORKING;
        return;
    }
    if (packet[0] == HCI_EVENT_META_GAP && size >= 33
        && hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
        if (gap_subevent_le_connection_complete_get_status(packet) != 0) return;
        uint16_t handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
        latency_link_t *link = find_link(handle);
        if (!link) {
            for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
                if (!links[i].connected) { link = &links[i]; break; }
            }
        }
        if (!link) {
            printf("[latency] connection telemetry table full\n");
            return;
        }
        memset(link, 0, sizeof(*link));
        link->connected = true;
        link->attempted = true;
        link->handle = handle;
        link->role = gap_subevent_le_connection_complete_get_role(packet);
        link->interval = gap_subevent_le_connection_complete_get_conn_interval(packet);
        link->latency = gap_subevent_le_connection_complete_get_conn_latency(packet);
        link->timeout = gap_subevent_le_connection_complete_get_supervision_timeout(packet);
        link->baseline_interval = link->interval;
        link->baseline_latency = link->latency;
        link->baseline_timeout = link->timeout;
        link->connected_at = btstack_run_loop_get_time_ms();
        radio_activity_at = link->connected_at;
        print_link(link);
    } else if (packet[0] == HCI_EVENT_LE_META && size >= 12
               && hci_event_le_meta_get_subevent_code(packet) == HCI_SUBEVENT_LE_CONNECTION_UPDATE_COMPLETE) {
        latency_link_t *link = find_link(hci_subevent_le_connection_update_complete_get_connection_handle(packet));
        if (!link) return;
        uint8_t status = hci_subevent_le_connection_update_complete_get_status(packet);
        link->pending = false;
        link->remote_pending = false;
        radio_activity_at = btstack_run_loop_get_time_ms();
        printf("[latency] connection update handle=0x%04x status=0x%02x\n", link->handle, status);
        if (status == 0) {
            link->interval = hci_subevent_le_connection_update_complete_get_conn_interval(packet);
            link->latency = hci_subevent_le_connection_update_complete_get_conn_latency(packet);
            link->timeout = hci_subevent_le_connection_update_complete_get_supervision_timeout(packet);
            if (!link->tuned) {
                link->baseline_interval = link->interval;
                link->baseline_latency = link->latency;
                link->baseline_timeout = link->timeout;
            }
            print_link(link);
        }
    } else if (packet[0] == HCI_EVENT_DISCONNECTION_COMPLETE && size >= 6 && packet[2] == 0) {
        latency_link_t *link = find_link(hci_event_disconnection_complete_get_connection_handle(packet));
        if (link) memset(link, 0, sizeof(*link));
        radio_activity_at = btstack_run_loop_get_time_ms();
    } else if ((packet[0] == HCI_EVENT_ENCRYPTION_CHANGE && size >= 6)
               || (packet[0] == HCI_EVENT_ENCRYPTION_KEY_REFRESH_COMPLETE && size >= 5)
               || (packet[0] == HCI_EVENT_CONNECTION_COMPLETE && size >= 13)) {
        radio_activity_at = btstack_run_loop_get_time_ms();
    } else if ((packet[0] == L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_REQUEST && size >= 12)
               || (packet[0] == HCI_EVENT_LE_META && size >= 13
                   && packet[2] == HCI_SUBEVENT_LE_REMOTE_CONNECTION_PARAMETER_REQUEST)) {
        uint16_t handle = little_endian_read_16(packet, packet[0] == HCI_EVENT_LE_META ? 3 : 2);
        latency_link_t *link = find_link(handle);
        radio_activity_at = btstack_run_loop_get_time_ms();
        if (link) {
            link->remote_pending = true;
            link->remote_request_at = radio_activity_at;
        }
    } else if (packet[0] == L2CAP_EVENT_CONNECTION_PARAMETER_UPDATE_RESPONSE && size >= 6) {
        latency_link_t *link = find_link(little_endian_read_16(packet, 2));
        if (link) {
            uint16_t result = little_endian_read_16(packet, 4);
            printf("[latency] PC update response handle=0x%04x result=0x%04x\n", link->handle, result);
            if (result) {
                link->pending = false;
                radio_activity_at = btstack_run_loop_get_time_ms();
            }
        }
    }
}

static void update_parameters(void) {
    uint32_t now = btstack_run_loop_get_time_ms();
    uint16_t active = hub_output_active_handle();
    if (active != observed_active) {
        observed_active = active;
        radio_activity_at = now;
    }
    bool pending = false;
    bool busy = hub_security_input_pairing_blocked() || hub_input_radio_busy();
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        latency_link_t *link = &links[i];
        if (!link->connected) continue;
        if (link->pending && (uint32_t)(now - link->request_at) >= LATENCY_REQUEST_TIMEOUT_MS) {
            link->pending = false;
            radio_activity_at = now;
            hub_perf_parameter_timeout(link->handle);
            printf("[latency] handle=0x%04x no applied update observed; current interval retained\n",
                   link->handle);
        }
        if (link->remote_pending
            && (uint32_t)(now - link->remote_request_at) >= LATENCY_REQUEST_TIMEOUT_MS) {
            link->remote_pending = false;
            radio_activity_at = now;
            printf("[latency] handle=0x%04x peer update observation window expired\n", link->handle);
        }
        pending |= link->pending || link->remote_pending;
        busy |= gap_encryption_key_size(link->handle) == 0;
    }
    if (busy) radio_activity_at = now;
    if (!working || busy || pending || (uint32_t)(now - radio_activity_at) < LATENCY_SETTLE_MS) return;

    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        latency_link_t *link = &links[i];
        if (!link->connected) continue;
        bool desired = fast_mode && ((link->role == HCI_ROLE_MASTER && hub_input_is_connection(link->handle))
            || (link->role == HCI_ROLE_SLAVE && link->handle == active));
        if (link->attempted && desired == link->requested_fast) continue;
        if (!desired && !link->tuned) {
            link->attempted = true;
            link->requested_fast = false;
            continue;
        }
        uint16_t minimum = desired ? LATENCY_FAST_INTERVAL_MIN : link->baseline_interval;
        /* Never ask an already-faster peer to slow down as part of "fast". */
        uint16_t maximum = desired
            ? (link->interval >= LATENCY_FAST_INTERVAL_MIN && link->interval < LATENCY_FAST_INTERVAL_MAX
                ? link->interval : LATENCY_FAST_INTERVAL_MAX)
            : link->baseline_interval;
        /* A 7.5--15 ms range lets the computer retain its existing 15 ms interval. */
        if (desired && link->role == HCI_ROLE_SLAVE) maximum = minimum;
        uint16_t latency = desired ? 0 : link->baseline_latency;
        uint16_t timeout = desired ? 200 : link->baseline_timeout;
        link->attempted = true;
        link->requested_fast = desired;
        if (link->interval >= minimum && link->interval <= maximum && link->latency == latency
            && (desired || link->timeout == timeout)) {
            if (!desired) link->tuned = false;
            ++link->satisfied;
            printf("[latency] handle=0x%04x %s parameters already satisfied; no request\n",
                   link->handle, desired ? "fast" : "baseline");
            continue;
        }
        ++link->requests;
        int status = link->role == HCI_ROLE_MASTER
            ? gap_update_connection_parameters(link->handle, minimum, maximum, latency, timeout)
            : gap_request_connection_parameter_update(link->handle, minimum, maximum, latency, timeout);
        radio_activity_at = now;
        hub_perf_parameter_request(link->handle, desired, minimum, maximum, latency, timeout, status);
        printf("[latency] %s request handle=0x%04x interval-units=%u..%u latency=%u result=0x%02x\n",
               desired ? "low-latency" : "restore-baseline", link->handle,
               minimum, maximum, latency, status);
        if (!status) {
            link->pending = true;
            link->request_at = now;
            link->tuned = desired;
        }
        /* Wait for completion (or timeout) and a quiet window before another link. */
        break;
    }
}

static void service(btstack_timer_source_t *source) {
    update_parameters();
    btstack_run_loop_set_timer(source, 250);
    btstack_run_loop_add_timer(source);
}

bool hub_latency_set_fast(bool enabled) {
    if (hub_security_input_pairing_blocked()) {
        printf("[latency] finish pairing before changing performance mode\n");
        return false;
    }
    if (enabled == fast_mode) {
        printf("[latency] mode already %s\n", enabled ? "fast" : "normal");
        return true;
    }
    if (!save_mode(enabled)) return false;
    if (enabled) {
        for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
            latency_link_t *link = &links[i];
            if (!link->connected || link->tuned || link->pending) continue;
            link->baseline_interval = link->interval;
            link->baseline_latency = link->latency;
            link->baseline_timeout = link->timeout;
        }
    }
    fast_mode = enabled;
    hub_output_set_low_latency(enabled);
    printf("[latency] mode=%s; radio parameter changes are requests, not guaranteed acceptance\n",
           enabled ? "fast" : "normal");
    return true;
}

void hub_latency_status(void) {
    printf("[latency] mode=%s; MCU queue timing is NOT end-to-end latency or radio delivery time\n",
           fast_mode ? "fast" : "normal");
    printf("[latency] updates serialized; quiet-window=%ums input-radio-busy=%u quiet-age=%" PRIu32 "ms\n",
           LATENCY_SETTLE_MS, hub_input_radio_busy(), btstack_run_loop_get_time_ms() - radio_activity_at);
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        if (links[i].connected) print_link(&links[i]);
    }
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        for (unsigned type = 0; type < 2; ++type) {
            input_timing_t *metric = &timing[i][type];
            if (!metric->count) continue;
            printf("[latency] input=%u type=%s reports=%" PRIu32 " gap-samples=%" PRIu32
                   " gap-us min=%" PRIu32 " mean=%" PRIu64 " max=%" PRIu32
                   " long-gaps-over-100ms=%" PRIu32 " (idle-or-stall; excluded from this mean)\n",
                   i + 1, type ? "mouse" : "keyboard",
                   metric->count, metric->samples, metric->minimum,
                   metric->samples ? metric->sum / metric->samples : 0,
                   metric->maximum, metric->idle_gaps);
        }
    }
    hub_output_latency_status();
}

void hub_latency_reset(void) {
    memset(timing, 0, sizeof(timing));
    hub_output_latency_reset();
    hub_perf_reset();
    printf("[latency] measurement counters reset; input state and links retained\n");
}

void hub_latency_init(void) {
    memset(links, 0, sizeof(links));
    memset(timing, 0, sizeof(timing));
    fast_mode = false;
    working = false;
    radio_activity_at = btstack_run_loop_get_time_ms();
    observed_active = HCI_CON_HANDLE_INVALID;
    load_mode();
    registration.callback = event_handler;
    hci_add_event_handler(&registration);
    l2cap_registration.callback = event_handler;
    l2cap_add_event_handler(&l2cap_registration);
    btstack_run_loop_set_timer_handler(&timer, service);
    btstack_run_loop_set_timer(&timer, 250);
    btstack_run_loop_add_timer(&timer);
}
