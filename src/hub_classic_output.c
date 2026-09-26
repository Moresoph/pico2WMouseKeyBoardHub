#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "ble/att_server.h"
#include "classic/hid_device.h"
#include "classic/sdp_server.h"
#include "hci.h"
#include "hub_classic_output.h"
#include "hub_classic_mux.h"
#include "hub_config.h"
#include "hub_hid_report.h"
#include "hub_input.h"
#include "hub_latency.h"
#include "hub_output.h"
#include "hub_output_store.h"
#include "hub_output_timer.h"
#include "hub_perf.h"
#include "hub_router.h"
#include "hub_security.h"
#include "hub.h"

#define QUEUE_LENGTH 64u
#define AUTH_TIMEOUT_MS 90000u
#define AUTH_START_DELAY_MS 500u
#define SERVICE_MS 100u
#define SEND_TIMEOUT_MS 1000u
#define PUMP_BUDGET 8u
#define SEND_PERIOD_US (HUB_CLASSIC_OUTPUT_7P5_TEST ? 7500u : 2500u)
#define RELEASE_KEYBOARD 1u
#define RELEASE_MOUSE 2u
#define RELEASE_BOTH 3u

typedef struct {
    uint8_t kind, buttons;
    bool timing_valid;
    uint32_t enqueued_us;
    uint8_t keyboard[8];
    int64_t x, y, wheel;
} classic_event_t;

typedef struct {
    uint32_t enqueued, submitted, errors, samples, max_us, bins[5];
    uint64_t sum_us;
} classic_report_metrics_t;

static hub_output_store_t store;
static bd_addr_t target;
static bool selected, started, connected, verified, suspended, disconnecting;
static bool sdp_registered;
static bool lease, pumping, credit, registered, pump_armed, low_latency;
static uint16_t hid_cid;
static uint8_t link_mode = 0xff, mode_status;
static uint16_t mode_interval;
static hci_con_handle_t handle = HCI_CON_HANDLE_INVALID;
static uint32_t lease_deadline, send_deadline;
static bool security_start_pending;
static uint32_t security_start_at;
static uint8_t releases, leds, last_keyboard[8], last_buttons, last_send_error;
static classic_event_t queue[QUEUE_LENGTH];
static unsigned head, count, highwater;
static classic_report_metrics_t metrics[2];
static uint32_t merges, overflows, safety_reports, retries, budget_yields;
static uint32_t bond_lookups, bond_lookup_max_us;
static uint64_t bond_lookup_sum_us;
static bool send_timing_valid;
static uint32_t send_requested_us, send_wait_samples, send_wait_max_us, send_wait_slow;
static uint64_t send_wait_sum_us;
static uint8_t send_blocked_by;
static struct { uint32_t samples, slow, max_us; } send_blockers[8];
static bool controller_buffers_seen, controller_le_buffers_seen;
static uint16_t controller_buffers, controller_packet_length, controller_le_packet_length;
static uint8_t controller_le_buffers;
static bool cadence_valid, gap_valid;
static uint32_t last_submit_us, last_submit_ms, gap_min_us, gap_max_us, gap_samples;
static uint64_t gap_sum_us;
static btstack_packet_callback_registration_t hci_registration;
static btstack_timer_source_t service_timer;
static uint8_t sdp_record[512];

static void pump(void);
static void update_security(void);
static void pump_after_yield(void);

static bool expired(uint32_t deadline) {
    return (int32_t)(btstack_run_loop_get_time_ms() - deadline) >= 0;
}

static bool store_valid(void) {
    if (!store.loaded || store.fault || store.selected != 0) return false;
    for (unsigned i = 1; i < HUB_OUTPUT_STORE_SLOTS; ++i)
        if (store.identities[i].reserved) return false;
    return !store.identities[0].reserved || store.identities[0].type == 0;
}

bool hub_classic_output_is_peer(const uint8_t address[6]) {
    return selected && address && !memcmp(target, address, sizeof(target))
        && !hub_input_is_classic_target(address);
}

static bool bonded(void) {
    link_key_t key;
    link_key_type_t type;
    uint32_t started_us = hub_latency_now_us();
    bool found = gap_get_link_key_for_bd_addr(target, key, &type);
    uint32_t elapsed_us = hub_latency_now_us() - started_us;
    ++bond_lookups;
    bond_lookup_sum_us += elapsed_us;
    if (elapsed_us > bond_lookup_max_us) bond_lookup_max_us = elapsed_us;
    memset(key, 0, sizeof(key));
    return found && (type == COMBINATION_KEY || type == CHANGED_COMBINATION_KEY
        || type == UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P192
        || type == AUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P192
        || type == UNAUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P256
        || type == AUTHENTICATED_COMBINATION_KEY_GENERATED_FROM_P256);
}

static bool saved(void) {
    return store_valid() && selected && hub_output_store_find(&store, 0, target) == 0;
}

static bool admitted(void) {
    /* Verification belongs to this encrypted ACL and is cleared on loss. The
       TLV-backed bond lookup must not scan uncached Flash on every report. */
    return store_valid() && hub_classic_output_is_peer(target)
        && ((saved() && (verified || bonded())) || (lease && !expired(lease_deadline)
            && hub_security_pairing_open()));
}

static void begin_lease(void) {
    if (lease) return;
    lease = true;
    lease_deadline = btstack_run_loop_get_time_ms() + AUTH_TIMEOUT_MS;
}

bool hub_classic_output_set_target(const uint8_t address[6]) {
    if (!started || !store_valid() || !hub_output_identity_valid(0, address)
        || hub_input_is_classic_target(address)) return false;
    if (store.identities[0].reserved && memcmp(store.identities[0].address, address, 6))
        return false;
    if (handle != HCI_CON_HANDLE_INVALID || hid_cid || lease || connected
        || hub_classic_mux_output_pending())
        return hub_classic_output_is_peer(address);
    memcpy(target, address, sizeof(target));
    selected = true;
    printf("[classic-output] explicit PC1 target=%s (no identity/bond write)\n",
           bd_addr_to_str(target));
    return true;
}

bool hub_classic_output_allow_pairing(const uint8_t address[6]) {
    if (!started || !store_valid() || !hub_classic_output_is_peer(address)
        || !hub_security_pairing_open() || disconnecting) return false;
    if (lease && expired(lease_deadline)) {
        if (handle != HCI_CON_HANDLE_INVALID || hid_cid) return false;
        lease = false;
    }
    begin_lease();
    return true;
}

bool hub_classic_output_accept_connection(const uint8_t address[6]) {
    if (!started || !store_valid() || !hub_classic_output_is_peer(address)
        || disconnecting) return false;
    if (!admitted() && !hub_classic_output_allow_pairing(address)) return false;
    /* The mux rejects duplicate PSMs; the same peer must open both HID channels. */
    return true;
}

static bool transport_ready(void) {
    return connected && verified && !suspended && !disconnecting && store_valid()
        && hub_classic_output_is_peer(target)
        && gap_encryption_key_size(handle) == 16
        && gap_security_level(handle) >= LEVEL_2
        && hub_classic_mux_output_ready(handle)
        && !hid_device_in_boot_protocol_mode(hid_cid);
}

bool hub_output_ready(unsigned slot) {
    return slot == 0 && transport_ready() && releases == 0;
}

unsigned hub_output_active_slot(void) { return 0; }
bool hub_output_switching(void) { return false; }
uint16_t hub_output_active_handle(void) {
    return hub_output_ready(0) ? handle : HCI_CON_HANDLE_INVALID;
}

bool hub_output_is_ready_connection(uint16_t connection_handle) {
    return connection_handle == handle && hub_output_ready(0);
}

bool hub_output_select(unsigned slot) {
    if (slot == 0) return true;
    printf("[classic-output] PC%u unavailable; singleton experiment supports PC1 only\n", slot + 1);
    return false;
}

static void clear_queue(void) {
    memset(queue, 0, sizeof(queue));
    head = count = 0;
}

static void require_release(void) {
    clear_queue();
    if (!releases) releases = RELEASE_BOTH;
}

static void lose_input(const char *reason) {
    require_release();
    printf("[classic-output] %s; suppress physical input until neutral\n", reason);
    hub_router_release_all();
}

static void close_link(const char *reason) {
    if (disconnecting) return;
    disconnecting = true;
    security_start_pending = false;
    verified = false;
    registered = credit = false;
    send_timing_valid = false;
    cadence_valid = gap_valid = false;
    lease = false;
    lose_input(reason);
    if (hid_cid) hid_device_disconnect(hid_cid);
    if (handle != HCI_CON_HANDLE_INVALID) {
        uint8_t status = gap_disconnect(handle);
        if (status) printf("[classic-output] disconnect status=0x%02x; retry pending\n", status);
    }
}

static void forget_connection(void) {
    bool had_link = connected || hid_cid || handle != HCI_CON_HANDLE_INVALID;
    connected = verified = suspended = disconnecting = lease = false;
    security_start_pending = false;
    registered = credit = false;
    send_timing_valid = false;
    cadence_valid = gap_valid = false;
    handle = HCI_CON_HANDLE_INVALID;
    link_mode = 0xff;
    mode_interval = mode_status = 0;
    hid_cid = 0;
    memset(last_keyboard, 0, sizeof(last_keyboard));
    last_buttons = leds = last_send_error = 0;
    releases = RELEASE_BOTH;
    require_release();
    if (had_link) lose_input("connection lost (release delivery unavailable)");
}

void hub_classic_output_pairing_changed(bool enabled) {
    gap_discoverable_control(enabled && started && hub_classic_output_is_peer(target) && store_valid()
                             && hub_security_pairing_open());
    if (!enabled) {
        lease = false;
        if (!saved() && (handle != HCI_CON_HANDLE_INVALID || hid_cid
            || hub_classic_mux_output_pending()))
            close_link("unenrolled pairing authorization revoked");
    }
}

bool hub_classic_output_connect(void) {
    printf("[classic-output] hub-initiated connect unsupported: stock HID singleton "
           "cannot safely use the incoming-only mux. Connect from the selected PC; "
           "new enrollment requires classic target then pair justworks\n");
    return false;
}

static uint32_t cadence_remaining(void) {
    if (releases || !cadence_valid
        || (uint32_t)(btstack_run_loop_get_time_ms() - last_submit_ms) > 100u) return 0;
    uint32_t elapsed = hub_latency_now_us() - last_submit_us;
    return elapsed < SEND_PERIOD_US ? SEND_PERIOD_US - elapsed : 0;
}

static uint8_t send_blockers_now(void) {
    uint8_t blocked = 0;
    if (hci_number_free_acl_slots_for_handle(handle) == 0) blocked |= 1u;
    if (hci_is_packet_buffer_reserved()) blocked |= 2u;
    /* The prepared-send predicate also tests credits; distinguish transport
       unavailability only when credits are available. This is a snapshot. */
    if (!(blocked & 1u) && !hci_can_send_prepared_acl_packet_now(handle)) blocked |= 4u;
    return blocked;
}

static void schedule_pump(void) {
    if (pump_armed || (!count && !releases)) return;
    pump_armed = true;
    uint32_t remaining = cadence_remaining();
    if (!hub_output_timer_schedule_us(remaining ? remaining : 1000u, pump_after_yield)) {
        pump_armed = false;
        close_link("microsecond send scheduler unavailable");
    }
}

static int32_t clamp(int64_t value, int32_t low, int32_t high) {
    return value < low ? low : value > high ? high : (int32_t)value;
}

static void measure_send(const classic_event_t *event) {
    classic_report_metrics_t *m = &metrics[event->kind == RELEASE_MOUSE];
    ++m->submitted;
    if (!event->timing_valid) return;
    uint32_t wait = hub_latency_now_us() - event->enqueued_us;
    ++m->samples;
    m->sum_us += wait;
    if (wait > m->max_us) m->max_us = wait;
    unsigned bin = wait < 1000 ? 0 : wait < 5000 ? 1 : wait < 10000 ? 2 : wait < 20000 ? 3 : 4;
    ++m->bins[bin];
    hub_perf_output_wait(handle, 0, event->kind == RELEASE_MOUSE, wait);
}

static bool send_one(void) {
    if (!transport_ready() || (!releases && !count)) return false;
    uint8_t kind = releases ? ((releases & RELEASE_KEYBOARD) ? RELEASE_KEYBOARD : RELEASE_MOUSE)
                            : queue[head].kind;
    classic_event_t *event = releases ? NULL : &queue[head];
    uint8_t packet[10] = {0xa1, kind};
    int32_t dx = 0, dy = 0, wheel = 0;
    unsigned length = kind == RELEASE_KEYBOARD ? 10 : 8;
    if (event) {
        if (kind == RELEASE_KEYBOARD) memcpy(&packet[2], event->keyboard, 8);
        else {
            dx = clamp(event->x, -32768, 32767);
            dy = clamp(event->y, -32768, 32767);
            wheel = clamp(event->wheel, -128, 127);
            packet[2] = event->buttons;
            little_endian_store_16(packet, 3, (uint16_t)dx);
            little_endian_store_16(packet, 5, (uint16_t)dy);
            packet[7] = (uint8_t)wheel;
        }
    }
    /* Stock hid_device_send_interrupt_message discards l2cap_send's status. The
       mux owns the current mapping; no SDK singleton internals are accessed. */
    uint8_t status = hub_classic_mux_send(packet, (uint16_t)length);
    if (status) {
        ++metrics[kind == RELEASE_MOUSE].errors;
        ++retries;
        if (status != last_send_error)
            printf("[classic-output] interrupt submit status=0x%02x; retained for retry\n", status);
        last_send_error = status;
        return false;
    }
    last_send_error = 0;
    if (kind == RELEASE_KEYBOARD) memcpy(last_keyboard, &packet[2], 8);
    else last_buttons = packet[2];
    if (!event) {
        releases &= (uint8_t)~kind;
        ++safety_reports;
    } else {
        uint32_t now_us = hub_latency_now_us();
        uint32_t now_ms = btstack_run_loop_get_time_ms();
        if (gap_valid && (uint32_t)(now_ms - last_submit_ms) < 60000u) {
            uint32_t gap = now_us - last_submit_us;
            if (!gap_samples || gap < gap_min_us) gap_min_us = gap;
            if (gap > gap_max_us) gap_max_us = gap;
            gap_sum_us += gap;
            ++gap_samples;
        }
        last_submit_us = now_us;
        last_submit_ms = now_ms;
        cadence_valid = gap_valid = true;
        measure_send(event);
        event->x -= dx;
        event->y -= dy;
        event->wheel -= wheel;
        if (kind == RELEASE_KEYBOARD || (!event->x && !event->y && !event->wheel)) {
            memset(event, 0, sizeof(*event));
            head = (head + 1) % QUEUE_LENGTH;
            --count;
        }
    }
    return true;
}

static void pump(void) {
    if (pumping || !transport_ready() || (!releases && !count)) return;
    pumping = true;
    unsigned budget = low_latency ? PUMP_BUDGET : 1;
    while (budget-- && transport_ready() && (releases || count)) {
        if (cadence_remaining()) break;
        if (!credit) {
            if (registered) break;
            registered = true;
            send_deadline = btstack_run_loop_get_time_ms() + SEND_TIMEOUT_MS;
            send_requested_us = hub_latency_now_us();
            send_timing_valid = true;
            send_blocked_by = send_blockers_now();
            hid_device_request_can_send_now_event(hid_cid);
            if (!credit) break;
        }
        credit = false;
        if (!send_one()) break;
    }
    pumping = false;
    if (!registered && transport_ready() && (releases || count)) {
        ++budget_yields;
        schedule_pump();
    }
}

static void pump_after_yield(void) {
    pump_armed = false;
    pump();
}

void hub_output_release_all(void) {
    require_release();
    pump();
}

static bool overflow(void) {
    ++overflows;
    lose_input("bounded output queue overflow");
    pump();
    return false;
}

static classic_event_t *enqueue(uint8_t kind) {
    if (count == QUEUE_LENGTH) {
        overflow();
        return NULL;
    }
    classic_event_t *event = &queue[(head + count++) % QUEUE_LENGTH];
    memset(event, 0, sizeof(*event));
    event->kind = kind;
    event->timing_valid = true;
    event->enqueued_us = hub_latency_now_us();
    ++metrics[kind == RELEASE_MOUSE].enqueued;
    if (count > highwater) highwater = count;
    return event;
}

bool hub_output_keyboard(const uint8_t report[8]) {
    if (!report || !hub_output_ready(0)) return false;
    classic_event_t *event = enqueue(RELEASE_KEYBOARD);
    if (!event) return false;
    memcpy(event->keyboard, report, 8);
    pump();
    return true;
}

bool hub_output_mouse(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel) {
    if (!hub_output_ready(0)) return false;
    if (!hub_hid_motion_bounded(dx, dy, wheel)) return overflow();
    buttons &= 0x1f;
    if (count && !wheel) {
        classic_event_t *tail = &queue[(head + count - 1) % QUEUE_LENGTH];
        /* Wheel events remain barriers: opposite detents must not cancel. */
        if (tail->kind == RELEASE_MOUSE && tail->buttons == buttons && !tail->wheel) {
            int64_t x = tail->x + dx, y = tail->y + dy;
            if (!hub_hid_motion_bounded(x, y, 0)) return overflow();
            tail->x = x;
            tail->y = y;
            ++metrics[1].enqueued;
            ++merges;
            pump();
            return true;
        }
    }
    classic_event_t *event = enqueue(RELEASE_MOUSE);
    if (!event) return false;
    event->buttons = buttons;
    event->x = dx;
    event->y = dy;
    event->wheel = wheel;
    pump();
    return true;
}

static void update_security(void) {
    if (!connected || disconnecting) return;
    bool secure = admitted() && gap_encryption_key_size(handle) == 16
        && gap_security_level(handle) >= LEVEL_2 && (verified || bonded())
        && hub_classic_mux_output_ready(handle);
    if (!secure) {
        if (verified) close_link("output security lost");
        return;
    }
    if (verified) return;
    if (!saved() && hub_output_store_claim(&store, 0, target) != 0) {
        close_link("PC1 identity persistence failed");
        return;
    }
    verified = true;
    lease = false;
    lose_input("PC1 secured; initial neutral reports required");
    printf("[classic-output] PC1 encrypted bonded HID; neutral reports pending\n");
    pump();
}

static int get_report(uint16_t cid, hid_report_type_t type, uint16_t id,
                      int *size, uint8_t *report) {
    *size = 0;
    if (cid != hid_cid || !transport_ready() || releases) return 0;
    if (type == HID_REPORT_TYPE_INPUT && id == 1) {
        *size = 8;
        memcpy(report, last_keyboard, 8);
    } else if (type == HID_REPORT_TYPE_INPUT && id == 2) {
        *size = 6;
        memset(report, 0, 6);
        report[0] = last_buttons; /* GET_REPORT must not replay relative movement. */
    } else if (type == HID_REPORT_TYPE_OUTPUT && id == 1) {
        *size = 1;
        report[0] = leds;
    } else return -1;
    return 1;
}

static void report_data(uint16_t cid, hid_report_type_t type, uint16_t id,
                        int size, uint8_t *report) {
    if (cid == hid_cid && transport_ready() && type == HID_REPORT_TYPE_OUTPUT
        && id == 1 && size == 1 && report) leds = report[0] & 0x1f;
}

static void set_report(uint16_t cid, hid_report_type_t type, int size, uint8_t *report) {
    if (report && size == 2) report_data(cid, type, report[0], 1, &report[1]);
}

static void hid_event(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (type != HCI_EVENT_PACKET || size < 5 || packet[0] != HCI_EVENT_HID_META) return;
    uint16_t cid = little_endian_read_16(packet, 3);
    if (packet[2] == HID_SUBEVENT_CONNECTION_OPENED) {
        if (size < 15) return;
        bd_addr_t address;
        hid_subevent_connection_opened_get_bd_addr(packet, address);
        if (!hub_classic_output_is_peer(address) || (hid_cid && hid_cid != cid)) return;
        if (hid_subevent_connection_opened_get_status(packet)) {
            hid_cid = cid;
            close_link("HID channel open failed");
            return;
        }
        hci_con_handle_t opened = hid_subevent_connection_opened_get_con_handle(packet);
        if (!admitted() || (handle != HCI_CON_HANDLE_INVALID && handle != opened)) {
            hid_device_disconnect(cid);
            return;
        }
        handle = opened;
        hid_cid = cid;
        connected = true;
        security_start_pending = false;
        releases = RELEASE_BOTH;
        require_release();
        gap_request_security_level(handle, LEVEL_2);
        update_security();
        return;
    }
    /* The pinned helper emits CLOSED with cid=0. Only an absent current channel
       mapping may retire this connection; an old close cannot retire a new one. */
    if (packet[2] == HID_SUBEVENT_CONNECTION_CLOSED) {
        if (connected && handle != HCI_CON_HANDLE_INVALID && !hub_classic_mux_output_ready(handle))
            forget_connection();
        return;
    }
    if (!connected || cid != hid_cid) return;
    switch (packet[2]) {
        case HID_SUBEVENT_CAN_SEND_NOW:
            if (!registered) break;
            if (send_timing_valid) {
                uint32_t wait = hub_latency_now_us() - send_requested_us;
                ++send_wait_samples;
                send_wait_sum_us += wait;
                if (wait > send_wait_max_us) send_wait_max_us = wait;
                if (wait >= 10000u) ++send_wait_slow;
                ++send_blockers[send_blocked_by].samples;
                if (wait >= 10000u) ++send_blockers[send_blocked_by].slow;
                if (wait > send_blockers[send_blocked_by].max_us)
                    send_blockers[send_blocked_by].max_us = wait;
            }
            send_timing_valid = false;
            registered = false;
            credit = true;
            if (pumping) break;
            credit = false;
            send_one();
            /* Do not reenter the stock helper before it restores its state. */
            if (transport_ready()) schedule_pump();
            break;
        case HID_SUBEVENT_SUSPEND:
            suspended = true;
            lose_input("host suspended");
            break;
        case HID_SUBEVENT_EXIT_SUSPEND:
            suspended = false;
            require_release();
            schedule_pump();
            break;
        case HID_SUBEVENT_VIRTUAL_CABLE_UNPLUG:
            close_link("host virtual cable unplug (identity/bond retained)");
            break;
        default:
            break;
    }
}

static void hci_event(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (type != HCI_EVENT_PACKET || size < 2) return;
    switch (packet[0]) {
        case HCI_EVENT_COMMAND_COMPLETE:
            if (size < 6 || packet[5]) break;
            switch (little_endian_read_16(packet, 3)) {
                case HCI_OPCODE_HCI_READ_BUFFER_SIZE:
                    if (size < 13) break;
                    controller_packet_length = little_endian_read_16(packet, 6);
                    controller_buffers = little_endian_read_16(packet, 9);
                    controller_buffers_seen = true;
                    break;
                case HCI_OPCODE_HCI_LE_READ_BUFFER_SIZE:
                    if (size < 9) break;
                    controller_le_packet_length = little_endian_read_16(packet, 6);
                    controller_le_buffers = packet[8];
                    controller_le_buffers_seen = true;
                    break;
                default:
                    break;
            }
            break;
        case HCI_EVENT_CONNECTION_COMPLETE: {
            if (size < 13 || hci_event_connection_complete_get_link_type(packet) != 1) break;
            bd_addr_t address;
            hci_event_connection_complete_get_bd_addr(packet, address);
            if (!hub_classic_output_is_peer(address)) break;
            if (hci_event_connection_complete_get_status(packet)) {
                if (!connected) forget_connection();
                break;
            }
            hci_con_handle_t next = hci_event_connection_complete_get_connection_handle(packet);
            if (handle != HCI_CON_HANDLE_INVALID && handle != next) break;
            if (!admitted() && !hub_classic_output_allow_pairing(address)) break;
            handle = next;
            link_mode = 0;
            mode_interval = mode_status = 0;
            begin_lease();
            /* The incoming PC may already be starting authentication. Give it
               first opportunity instead of immediately colliding with it. */
            security_start_pending = true;
            security_start_at = btstack_run_loop_get_time_ms() + AUTH_START_DELAY_MS;
            break;
        }
        case HCI_EVENT_MODE_CHANGE:
            if (size < 8 || handle == HCI_CON_HANDLE_INVALID
                || hci_event_mode_change_get_handle(packet) != handle) break;
            mode_status = hci_event_mode_change_get_status(packet);
            if (!mode_status) {
                link_mode = hci_event_mode_change_get_mode(packet);
                mode_interval = hci_event_mode_change_get_interval(packet);
            }
            printf("[classic-output] PC1 mode-change status=0x%02x mode=%u interval=%u(0.625ms)\n",
                   mode_status, link_mode, mode_interval);
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            if (size >= 6 && !hci_event_disconnection_complete_get_status(packet)
                && hci_event_disconnection_complete_get_connection_handle(packet) == handle)
                forget_connection();
            break;
        case GAP_EVENT_SECURITY_LEVEL:
            if (size < 6 || gap_event_security_level_get_handle(packet) != handle) break;
            if (gap_event_security_level_get_status(packet)
                || (verified && gap_event_security_level_get_security_level(packet) < LEVEL_2))
                close_link("Level2 authentication/encryption failed");
            else if (gap_event_security_level_get_security_level(packet) >= LEVEL_2)
                update_security();
            break;
        case HCI_EVENT_ENCRYPTION_CHANGE:
            if (size < 6 || hci_event_encryption_change_get_connection_handle(packet) != handle) break;
            if (hci_event_encryption_change_get_status(packet)
                || !hci_event_encryption_change_get_encryption_enabled(packet))
                close_link("encryption disabled");
            else update_security();
            break;
        case HCI_EVENT_ENCRYPTION_CHANGE_V2:
            if (size < 7 || hci_event_encryption_change_v2_get_connection_handle(packet) != handle) break;
            if (hci_event_encryption_change_v2_get_status(packet)
                || !hci_event_encryption_change_v2_get_encryption_enabled(packet)
                || hci_event_encryption_change_v2_get_encryption_key_size(packet) != 16)
                close_link("encryption/key size invalid");
            else update_security();
            break;
        case HCI_EVENT_ENCRYPTION_KEY_REFRESH_COMPLETE:
            if (size < 5 || hci_event_encryption_key_refresh_complete_get_handle(packet) != handle) break;
            if (hci_event_encryption_key_refresh_complete_get_status(packet))
                close_link("encryption key refresh failed");
            else update_security();
            break;
        case HCI_EVENT_AUTHENTICATION_COMPLETE:
            if (size >= 5 && hci_event_authentication_complete_get_connection_handle(packet) == handle
                && hci_event_authentication_complete_get_status(packet))
                close_link("authentication failed");
            break;
        default:
            break;
    }
}

static void service(btstack_timer_source_t *timer) {
    (void)timer;
    if (!hub_security_pairing_open()) {
        if (lease) hub_classic_output_pairing_changed(false);
        else gap_discoverable_control(0);
    }
    if (disconnecting) {
        if (handle != HCI_CON_HANDLE_INVALID) {
            uint8_t status = gap_disconnect(handle);
            if (status == ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER && !hub_classic_mux_output_pending())
                forget_connection();
        } else if (!hub_classic_mux_output_pending()) forget_connection();
    } else if (lease && expired(lease_deadline)) {
        if (handle != HCI_CON_HANDLE_INVALID || hid_cid) close_link("authorization timed out");
        else lease = false;
    } else {
        if (security_start_pending && expired(security_start_at)) {
            security_start_pending = false;
            if (gap_encryption_key_size(handle) != 16 || gap_security_level(handle) < LEVEL_2)
                gap_request_security_level(handle, LEVEL_2);
        }
        update_security();
        if (verified && (!hub_classic_output_is_peer(target)
            || !hub_classic_mux_output_ready(handle))) close_link("HID channel/target lost");
        if (registered && expired(send_deadline)) {
            registered = false;
            send_timing_valid = false;
            ++retries;
            printf("[classic-output] CAN_SEND_NOW timeout; retrying retained report\n");
        }
        pump();
    }
    btstack_run_loop_set_timer(&service_timer, SERVICE_MS);
    btstack_run_loop_add_timer(&service_timer);
}

void hub_output_init(void) {
    /* HID peripherals may also initiate ATT transactions. Without a server,
       BTstack drops their requests even though our GATT client is running. */
    att_server_init(profile_data, NULL, NULL);
    memset(&store, 0, sizeof(store));
    memset(target, 0, sizeof(target));
    selected = started = connected = verified = suspended = disconnecting = false;
    security_start_pending = false;
    lease = pumping = credit = registered = pump_armed = low_latency = false;
    cadence_valid = gap_valid = false;
    handle = HCI_CON_HANDLE_INVALID;
    link_mode = 0xff;
    mode_interval = mode_status = 0;
    hid_cid = 0;
    releases = RELEASE_BOTH;
    leds = last_buttons = last_send_error = 0;
    memset(last_keyboard, 0, sizeof(last_keyboard));
    clear_queue();
    hub_output_latency_reset();
    controller_buffers_seen = controller_le_buffers_seen = false;
    controller_buffers = controller_packet_length = controller_le_packet_length = 0;
    controller_le_buffers = 0;
    hid_device_init(false, sizeof(hub_hid_report_descriptor), hub_hid_report_descriptor);
    hid_device_register_packet_handler(hid_event);
    hid_device_register_report_request_callback(get_report);
    hid_device_register_set_report_callback(set_report);
    hid_device_register_report_data_callback(report_data);
    hid_device_accept_truncated_hid_reports(false);
    const hid_sdp_record_t record = {
        .hid_device_subclass = 0xc0,
        .hid_virtual_cable = 1,
        .hid_reconnect_initiate = 0,
        .hid_normally_connectable = true,
        .hid_boot_device = false,
        .hid_supervision_timeout = 0x0c80,
        .hid_ssr_host_max_latency = 0xffff,
        .hid_ssr_host_min_timeout = 0xffff,
        .hid_descriptor = hub_hid_report_descriptor,
        .hid_descriptor_size = sizeof(hub_hid_report_descriptor),
        .device_name = HUB_DEVICE_NAME
    };
    hid_create_sdp_record(sdp_record, 0x10001, &record);
    uint8_t status = sdp_register_service(sdp_record);
    sdp_registered = status == ERROR_CODE_SUCCESS;
    if (status) printf("[classic-output] SDP registration failed status=0x%02x\n", status);
    hci_registration.callback = hci_event;
    hci_add_event_handler(&hci_registration);
    btstack_run_loop_set_timer_handler(&service_timer, service);
}

void hub_output_start(void) {
    if (started) return;
    if (!sdp_registered) {
        printf("[classic-output] startup disabled: HID SDP service is unavailable\n");
        return;
    }
    started = true;
    hub_output_store_load(&store);
    if (!store_valid()) {
        store.fault = true;
        printf("[classic-output] PHC1 invalid/non-PC1 reservations; output locked\n");
    } else if (store.identities[0].reserved) {
        memcpy(target, store.identities[0].address, sizeof(target));
        selected = true;
    }
    gap_connectable_control(1);
    hub_classic_output_pairing_changed(hub_security_pairing_open());
    btstack_run_loop_set_timer(&service_timer, SERVICE_MS);
    btstack_run_loop_add_timer(&service_timer);
    printf("[classic-output] one PC1, report protocol only, Level2 encrypted bonded output\n");
}

void hub_output_set_low_latency(bool enabled) {
    low_latency = enabled;
    pump();
}

void hub_output_mouse_buffer_status(void) {
    printf("[mouse-buffer] unavailable on Classic output; BLE-only experiment\n");
}

bool hub_output_set_mouse_buffering(bool enabled) {
    (void)enabled;
    hub_output_mouse_buffer_status();
    return false;
}

void hub_output_latency_reset(void) {
    memset(metrics, 0, sizeof(metrics));
    merges = overflows = safety_reports = retries = budget_yields = 0;
    bond_lookups = bond_lookup_max_us = 0;
    bond_lookup_sum_us = 0;
    send_timing_valid = false;
    send_wait_samples = send_wait_max_us = send_wait_slow = 0;
    send_wait_sum_us = 0;
    memset(send_blockers, 0, sizeof(send_blockers));
    gap_valid = false;
    gap_min_us = gap_max_us = gap_samples = 0;
    gap_sum_us = 0;
    highwater = count;
    for (unsigned i = 0; i < count; ++i)
        queue[(head + i) % QUEUE_LENGTH].timing_valid = false;
}

void hub_output_latency_status(void) {
    printf("[latency classic-output] controller ACL capacity seen=%u reported=%u"
           " host-cap=%u effective=%u packet-length=%u;"
           " LE seen=%u reported=%u packet-length=%u (LE count 0 shares Classic pool)\n",
           controller_buffers_seen, controller_buffers, MAX_NR_CONTROLLER_ACL_BUFFERS,
           controller_buffers < MAX_NR_CONTROLLER_ACL_BUFFERS
               ? controller_buffers : MAX_NR_CONTROLLER_ACL_BUFFERS,
           controller_packet_length, controller_le_buffers_seen,
           controller_le_buffers, controller_le_packet_length);
    printf("[latency classic-output] PC1 link-mode=%u (0=active,1=hold,2=sniff,3=park,255=no-link)"
           " interval=%u(0.625ms) last-mode-status=0x%02x; not report pacing\n",
           link_mode, mode_interval, mode_status);
    printf("[latency classic-output] software-period=%uus max=%u.%03u reports/s total keyboard+mouse; "
           "event-driven when idle, safety releases bypass pacing; NOT an air-link interval\n",
           SEND_PERIOD_US, 1000000u / SEND_PERIOD_US, (1000000000u / SEND_PERIOD_US) % 1000u);
    printf("[latency classic-output] ordinary submit-gap samples=%" PRIu32
           " min/avg/max=%" PRIu32 "/%" PRIu64 "/%" PRIu32
           "us (includes idle gaps <60s, not radio delivery)\n",
           gap_samples, gap_min_us, gap_samples ? gap_sum_us / gap_samples : 0, gap_max_us);
    printf("[latency classic-output] PC1 MCU enqueue->successful L2CAP submission us; "
           "NOT ATT, radio delivery or end-to-end; mode=%s queued=%u peak=%u "
           "merges=%" PRIu32 " overflows=%" PRIu32 " releases=%" PRIu32
           " retries=%" PRIu32 " yields=%" PRIu32 "\n",
           low_latency ? "fast" : "baseline", count, highwater, merges, overflows,
           safety_reports, retries, budget_yields);
    printf("[latency classic-output] bond-lookup calls=%" PRIu32 " sum-us=%" PRIu64
           " max-us=%" PRIu32 "; verified ACL reports do not rescan Flash\n",
           bond_lookups, bond_lookup_sum_us, bond_lookup_max_us);
    printf("[latency classic-output] request->CAN_SEND_NOW samples=%" PRIu32
           " mean/max=%" PRIu64 "/%" PRIu32 "us slow(>=10ms)=%" PRIu32
           "; includes safety reports; stack/controller availability, NOT radio delivery\n",
           send_wait_samples, send_wait_samples ? send_wait_sum_us / send_wait_samples : 0,
           send_wait_max_us, send_wait_slow);
    for (unsigned i = 0; i < 8; ++i) {
        if (!send_blockers[i].samples) continue;
        printf("[latency classic-output] send-request snapshot mask=%u"
               " (1=no-credit,2=shared-buffer-reserved,4=transport-unavailable,0=HCI-ready)"
               " samples=%" PRIu32 " slow(>=10ms)=%" PRIu32 " max=%" PRIu32 "us\n",
               i, send_blockers[i].samples, send_blockers[i].slow, send_blockers[i].max_us);
    }
    for (unsigned i = 0; i < 2; ++i) {
        const classic_report_metrics_t *m = &metrics[i];
        printf("[latency classic-output] %s enqueue=%" PRIu32 " submitted=%" PRIu32
               " errors=%" PRIu32 " samples=%" PRIu32 " sum-us=%" PRIu64
               " max-us=%" PRIu32 " bins(<1/5/10/20/>=20ms)=%" PRIu32
               "/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "\n",
               i ? "mouse" : "keyboard", m->enqueued, m->submitted, m->errors,
               m->samples, m->sum_us, m->max_us, m->bins[0], m->bins[1], m->bins[2],
               m->bins[3], m->bins[4]);
    }
}

void hub_output_status(void) {
    printf("[classic-output] PC1 period-us=%u target=%s saved=%u PHC1-fault=%u "
           "hid=0x%04x handle=0x%04x ready=%u encrypted-key=%u suspend=%u "
           "release=0x%02x queued=%u; PC2..PC4 unavailable\n",
           SEND_PERIOD_US, selected ? bd_addr_to_str(target) : "(explicit target required)", saved(), store.fault,
           hid_cid, handle, hub_output_ready(0),
           handle == HCI_CON_HANDLE_INVALID ? 0 : gap_encryption_key_size(handle),
           suspended, releases, count);
}
