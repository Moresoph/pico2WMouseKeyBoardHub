#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "hub_config.h"
#include "hub_hid_report.h"
#include "hub_latency.h"
#include "hub_perf.h"
#include "hub_output.h"
#include "hub_output_store.h"
#include "hub_output_timer.h"
#include "hub_router.h"
#include "hub_security.h"
#include "hub.h"

#define OUTPUT_QUEUE_LENGTH 64u
#define OUTPUT_AUTH_TIMEOUT_MS 90000u
#define OUTPUT_SERVICE_INTERVAL_MS 5u
#define OUTPUT_PUMP_BUDGET 8u
#define OUTPUT_PUMP_YIELD_MS 1u
#define MOUSE_RESERVE_SLOTS 4u
#define MOUSE_HOLD_US 7500u
#define MOUSE_IDLE_MS 100u
#define RELEASE_KEYBOARD 1u
#define RELEASE_MOUSE 2u
#define RELEASE_BOTH (RELEASE_KEYBOARD | RELEASE_MOUSE)
#define KEYBOARD_HANDLE ATT_CHARACTERISTIC_2A4D_01_VALUE_HANDLE
#define KEYBOARD_CCCD ATT_CHARACTERISTIC_2A4D_01_CLIENT_CONFIGURATION_HANDLE
#define LED_HANDLE ATT_CHARACTERISTIC_2A4D_02_VALUE_HANDLE
#define MOUSE_HANDLE ATT_CHARACTERISTIC_2A4D_03_VALUE_HANDLE
#define MOUSE_CCCD ATT_CHARACTERISTIC_2A4D_03_CLIENT_CONFIGURATION_HANDLE

typedef struct {
    uint8_t kind;
    bool timing_valid;
    bool motion_only;
    bool first_motion;
    uint32_t enqueued_us;
    union {
        uint8_t keyboard[8];
        struct {
            uint8_t buttons;
            int32_t x, y, wheel;
        } mouse;
    } value;
} output_event_t;

typedef struct {
    uint32_t enqueued, notified, wait_count, wait_max_us;
    uint64_t wait_sum_us;
    uint32_t wait_bins[5];
    unsigned queue_highwater;
    uint32_t notify_errors;
} output_report_metrics_t;

typedef struct {
    output_report_metrics_t reports[2];
    unsigned queue_highwater;
    uint32_t mouse_merges, releases, release_errors, registration_errors, budget_yields;
    uint32_t motion_holds, motion_deadline_sends;
} output_metrics_t;

typedef struct {
    bool occupied;
    bool retiring;
    bool disconnecting;
    bool rejected;
    bool authentication_complete;
    bool identity_authenticated;
    bool suspended;
    bool send_registered;
    bool pumping;
    bool continuation_pending;
    bool yield_pending;
    bool retry_pending;
    bool ready_logged;
    bool motion_seen;
    bool motion_waiting;
    uint32_t last_motion_ms;
    uint8_t key_size;
    uint8_t address_type;
    uint8_t leds;
    uint8_t release_pending;
    uint8_t last_send_error;
    uint16_t keyboard_cccd;
    uint16_t mouse_cccd;
    hci_con_handle_t handle;
    bd_addr_t address;
    uint32_t auth_deadline;
    uint32_t sent;
    uint32_t overflows;
    uint8_t last_keyboard[8];
    uint8_t last_mouse[6];
    output_event_t queue[OUTPUT_QUEUE_LENGTH];
    unsigned head;
    unsigned count;
    unsigned queued[2];
    unsigned slot;
    btstack_context_callback_registration_t send_registration;
} computer_t;

/* Physical contexts never move, including while their registration is queued in
   ATT. Persistent logical slots point at contexts only after bond verification. */
static computer_t computers[HUB_COMPUTER_COUNT];
static computer_t *slot_connections[HUB_COMPUTER_COUNT];
static hub_output_store_t output_store;
static btstack_packet_callback_registration_t hci_registration;
static btstack_packet_callback_registration_t sm_registration;
static btstack_timer_source_t service_timer;
static btstack_timer_source_t pump_timer;
static btstack_context_callback_registration_t pump_registration;
static output_metrics_t output_metrics[HUB_COMPUTER_COUNT];
static bool low_latency;
static bool mouse_buffering, motion_timer_armed;
static uint32_t motion_timer_due_us;
static bool continuation_registered;
static bool pump_timer_armed;
static unsigned active_slot;
static bool started;
static bool switch_pending;
static unsigned switch_old_slot, switch_target_slot;
static hci_con_handle_t switch_old_handle;
static uint32_t switch_wait_log;
static bool advertising_requested;
static uint8_t advertising_status;

static void request_send(computer_t *computer);
static void progress_switch(void);
static void schedule_continuation(computer_t *computer);
static void motion_timer_ready(void);

static void cancel_motion_timer(void) {
    if (motion_timer_armed) hub_output_timer_cancel();
    motion_timer_armed = false;
}

static void clear_motion_wait(computer_t *computer) {
    computer->motion_waiting = false;
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i)
        if (computers[i].motion_waiting) return;
    cancel_motion_timer();
}

static unsigned slot_of(const computer_t *computer) {
    return computer->slot;
}

static computer_t *computer_for_slot(unsigned slot) {
    return slot < HUB_COMPUTER_COUNT ? slot_connections[slot] : NULL;
}

static computer_t *computer_for_handle(hci_con_handle_t handle) {
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) {
        if (computers[i].occupied && computers[i].handle == handle) return &computers[i];
    }
    return NULL;
}

static bool secure(const computer_t *computer) {
    return computer != NULL && computer->occupied && !computer->disconnecting
        && !computer->rejected && computer->identity_authenticated
        && output_store.loaded && !output_store.fault
        && computer_for_slot(computer->slot) == computer
        && gap_encryption_key_size(computer->handle) >= 16;
}

static bool transport_ready(const computer_t *computer) {
    return secure(computer) && !computer->suspended
        && computer->keyboard_cccd == 1 && computer->mouse_cccd == 1;
}

bool hub_output_ready(unsigned slot) {
    computer_t *computer = computer_for_slot(slot);
    return transport_ready(computer) && computer->release_pending == 0;
}

bool hub_output_is_ready_connection(uint16_t handle) {
    computer_t *computer = computer_for_handle(handle);
    return transport_ready(computer) && computer->release_pending == 0;
}

unsigned hub_output_active_slot(void) {
    return active_slot;
}

bool hub_output_switching(void) {
    return switch_pending;
}

uint16_t hub_output_active_handle(void) {
    computer_t *computer = computer_for_slot(active_slot);
    return !switch_pending && hub_output_ready(active_slot)
        ? computer->handle : HCI_CON_HANDLE_INVALID;
}

static void clear_queue(computer_t *computer) {
    clear_motion_wait(computer);
    computer->motion_seen = false;
    memset(computer->queue, 0, sizeof(computer->queue));
    computer->head = 0;
    computer->count = 0;
    memset(computer->queued, 0, sizeof(computer->queued));
}

static void require_release(computer_t *computer) {
    if (computer == NULL) return;
    clear_queue(computer);
    /* No new input is accepted until both releases finish. Preserve partial
       progress when the router repeats release_all during congestion. */
    if (computer->occupied && computer->release_pending == 0)
        computer->release_pending = RELEASE_BOTH;
}

static void active_readiness_lost(const computer_t *computer, const char *reason) {
    if (slot_of(computer) != active_slot) return;
    printf("[output] active PC%u %s; suppressing physical input until neutral\n",
           slot_of(computer) + 1, reason);
    /* Call only after invalidating the peer and its queued input. The router
       calls output_release_all, which never calls this loss hook recursively. */
    hub_router_release_all();
}

static void log_readiness(computer_t *computer) {
    if (computer->slot >= HUB_COMPUTER_COUNT) return;
    bool ready = hub_output_ready(slot_of(computer));
    if (ready == computer->ready_logged) return;
    computer->ready_logged = ready;
    printf("[output] PC%u handle=0x%04x ready=%u encrypted-key=%u "
           "notify(k/m)=%u/%u suspend=%u release=0x%02x\n",
           slot_of(computer) + 1, computer->handle, ready,
           gap_encryption_key_size(computer->handle), computer->keyboard_cccd,
           computer->mouse_cccd, computer->suspended, computer->release_pending);
}

static void progress_switch(void) {
    if (!switch_pending) return;
    computer_t *old = computer_for_slot(switch_old_slot);
    bool same_link = old != NULL && old->occupied && old->handle == switch_old_handle;
    if (same_link && old->release_pending != 0 && transport_ready(old)) return;
    if (same_link && old->release_pending != 0) {
        printf("[output] switch: PC%u cannot receive both releases "
               "(security/subscription/suspend); neutral reports retained, no input retained\n",
               switch_old_slot + 1);
    } else if (!same_link && switch_old_handle != HCI_CON_HANDLE_INVALID) {
        printf("[output] switch: old PC%u link disappeared; release delivery impossible\n",
               switch_old_slot + 1);
    }
    /* Never fall back to the old or another PC if the requested target vanished. */
    active_slot = switch_target_slot;
    switch_pending = false;
    printf("[output] selected PC%u ready=%u; no computers disconnected by switch\n",
           active_slot + 1, hub_output_ready(active_slot));
}

static uint8_t sendable_release(const computer_t *computer) {
    if (!secure(computer) || computer->suspended) return 0;
    if ((computer->release_pending & RELEASE_KEYBOARD) && computer->keyboard_cccd == 1)
        return RELEASE_KEYBOARD;
    if ((computer->release_pending & RELEASE_MOUSE) && computer->mouse_cccd == 1)
        return RELEASE_MOUSE;
    return 0;
}

static int32_t clamp_delta(int32_t value, int32_t low, int32_t high) {
    return value < low ? low : value > high ? high : value;
}

static void pop_event(computer_t *computer) {
    --computer->queued[computer->queue[computer->head].kind == RELEASE_KEYBOARD ? 0 : 1];
    memset(&computer->queue[computer->head], 0, sizeof(output_event_t));
    computer->head = (computer->head + 1) % OUTPUT_QUEUE_LENGTH;
    --computer->count;
}

static void measure_enqueue(computer_t *computer, output_event_t *event, bool merged) {
    output_metrics_t *metrics = &output_metrics[computer->slot];
    unsigned kind = event->kind == RELEASE_KEYBOARD ? 0 : 1;
    output_report_metrics_t *report = &metrics->reports[kind];
    ++report->enqueued;
    if (merged) {
        ++metrics->mouse_merges;
        return; /* A merged/split mouse event retains the oldest enqueue time. */
    }
    event->enqueued_us = hub_latency_now_us();
    event->timing_valid = true;
    ++computer->queued[kind];
    if (computer->queued[kind] > report->queue_highwater)
        report->queue_highwater = computer->queued[kind];
    if (computer->count > metrics->queue_highwater)
        metrics->queue_highwater = computer->count;
}

static void measure_notify(computer_t *computer, const output_event_t *event) {
    output_report_metrics_t *metrics =
        &output_metrics[computer->slot].reports[event->kind == RELEASE_KEYBOARD ? 0 : 1];
    ++metrics->notified;
    if (!event->timing_valid) return;
    uint32_t wait = hub_latency_now_us() - event->enqueued_us;
    hub_perf_output_wait(computer->handle, computer->slot, event->kind == RELEASE_MOUSE, wait);
    ++metrics->wait_count;
    metrics->wait_sum_us += wait;
    if (wait > metrics->wait_max_us) metrics->wait_max_us = wait;
    unsigned bin = wait < 1000 ? 0 : wait < 5000 ? 1 : wait < 10000 ? 2
        : wait < 20000 ? 3 : 4;
    ++metrics->wait_bins[bin];
}

static bool reserved_motion(const computer_t *computer) {
    if (!mouse_buffering || !computer->count || computer->release_pending) return false;
    const output_event_t *head = &computer->queue[computer->head];
    if (!head->motion_only || head->first_motion) return false;
    /* A later key/button/wheel is a FIFO barrier: flush earlier motion first. */
    for (unsigned i = 1; i < computer->count; ++i)
        if (!computer->queue[(computer->head + i) % OUTPUT_QUEUE_LENGTH].motion_only) return false;
    return true;
}

static bool hold_motion(computer_t *computer) {
    if (!mouse_buffering) return false;
    uint32_t now = hub_latency_now_us();
    uint32_t age = computer->count ? now - computer->queue[computer->head].enqueued_us : 0;
    if (!reserved_motion(computer) || age >= MOUSE_HOLD_US
        || hci_number_free_acl_slots_for_handle(computer->handle) > (int)MOUSE_RESERVE_SLOTS) {
        clear_motion_wait(computer);
        return false;
    }
    if (!computer->motion_waiting) ++output_metrics[computer->slot].motion_holds;
    computer->motion_waiting = true;
    uint32_t delay = MOUSE_HOLD_US - age;
    uint32_t due = now + delay;
    if (motion_timer_armed && (int32_t)(motion_timer_due_us - due) <= 0) return true;
    cancel_motion_timer();
    motion_timer_due_us = due;
    motion_timer_armed = hub_output_timer_schedule_us(delay, motion_timer_ready);
    if (motion_timer_armed) return true;
    mouse_buffering = false;
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) computers[i].motion_waiting = false;
    printf("[mouse-buffer] ERROR: deadline timer unavailable; experiment disabled,"
           " baseline sending resumed\n");
    return false;
}

static void can_send(void *context) {
    computer_t *computer = context;
    computer->send_registered = false;
    if (!computer->occupied) return;
    uint8_t report[8] = {0};
    uint8_t kind = sendable_release(computer);
    bool release = kind != 0;
    output_event_t *event = NULL;
    int32_t dx = 0, dy = 0, wheel = 0;
    if (!release) {
        if (!hub_output_ready(slot_of(computer)) || switch_pending
            || slot_of(computer) != active_slot || computer->count == 0) return;
        /* Credits may have changed since the ATT callback was requested. */
        if (hold_motion(computer)) return;
        event = &computer->queue[computer->head];
        kind = event->kind;
        if (kind == RELEASE_KEYBOARD) {
            memcpy(report, event->value.keyboard, 8);
        } else {
            dx = clamp_delta(event->value.mouse.x, -32768, 32767);
            dy = clamp_delta(event->value.mouse.y, -32768, 32767);
            wheel = clamp_delta(event->value.mouse.wheel, -128, 127);
            report[0] = event->value.mouse.buttons;
            little_endian_store_16(report, 1, (uint16_t)dx);
            little_endian_store_16(report, 3, (uint16_t)dy);
            report[5] = (uint8_t)wheel;
        }
    }
    bool deadline_send = !release && reserved_motion(computer)
        && (uint32_t)(hub_latency_now_us() - event->enqueued_us) >= MOUSE_HOLD_US
        && hci_number_free_acl_slots_for_handle(computer->handle) <= (int)MOUSE_RESERVE_SLOTS;
    uint8_t status = att_server_notify(computer->handle,
        kind == RELEASE_KEYBOARD ? KEYBOARD_HANDLE : MOUSE_HANDLE,
        report, kind == RELEASE_KEYBOARD ? 8 : 6);
    if (status != ERROR_CODE_SUCCESS) {
        if (release) ++output_metrics[computer->slot].release_errors;
        else ++output_metrics[computer->slot].reports[kind == RELEASE_KEYBOARD ? 0 : 1].notify_errors;
        computer->retry_pending = true;
        if (computer->last_send_error != status) {
            printf("[output] PC%u handle=0x%04x notify failed=0x%02x; retry pending\n",
                   slot_of(computer) + 1, computer->handle, status);
        }
        computer->last_send_error = status;
        return;
    }
    computer->last_send_error = 0;
    computer->retry_pending = false;
    if (deadline_send) ++output_metrics[computer->slot].motion_deadline_sends;
    ++computer->sent;
    if (kind == RELEASE_KEYBOARD) memcpy(computer->last_keyboard, report, 8);
    else memcpy(computer->last_mouse, report, 6);
    if (release) {
        ++output_metrics[computer->slot].releases;
        computer->release_pending &= (uint8_t)~kind;
    } else if (kind == RELEASE_KEYBOARD) {
        measure_notify(computer, event);
        pop_event(computer);
    } else {
        measure_notify(computer, event);
        event->first_motion = false;
        event->value.mouse.x -= dx;
        event->value.mouse.y -= dy;
        event->value.mouse.wheel -= wheel;
        if (event->value.mouse.x == 0 && event->value.mouse.y == 0
            && event->value.mouse.wheel == 0) pop_event(computer);
    }
    progress_switch();
    log_readiness(computer);
    /* ATT registration can invoke this callback synchronously. Never register
       again on its stack; an outer bounded loop or deferred worker continues. */
    if (low_latency && !computer->pumping) schedule_continuation(computer);
}

static bool send_work(const computer_t *computer) {
    if (computer == NULL || !computer->occupied) return false;
    bool input_pending = !switch_pending && slot_of(computer) == active_slot
        && computer->count != 0 && hub_output_ready(slot_of(computer));
    return sendable_release(computer) || input_pending;
}

static void schedule_continuation(computer_t *computer) {
    if (!send_work(computer) || computer->send_registered
        || computer->retry_pending || computer->yield_pending) return;
    computer->continuation_pending = true;
    if (continuation_registered) return;
    continuation_registered = true;
    btstack_run_loop_execute_on_main_thread(&pump_registration);
}

static void defer_pump(computer_t *computer) {
    computer->yield_pending = true;
    ++output_metrics[computer->slot].budget_yields;
    if (pump_timer_armed) return;
    pump_timer_armed = true;
    btstack_run_loop_set_timer(&pump_timer, OUTPUT_PUMP_YIELD_MS);
    btstack_run_loop_add_timer(&pump_timer);
}

static void request_send(computer_t *computer) {
    if (computer == NULL || computer->pumping || computer->send_registered
        || (low_latency && (computer->retry_pending || computer->yield_pending))) return;
    computer->pumping = true;
    unsigned budget = low_latency ? OUTPUT_PUMP_BUDGET : 1u;
    while (budget && send_work(computer)) {
        if (!sendable_release(computer) && hold_motion(computer)) break;
        --budget;
        uint32_t sent_before = computer->sent;
        computer->send_registered = true;
        uint8_t status = att_server_request_to_send_notification(
            &computer->send_registration, computer->handle);
        if (status != ERROR_CODE_SUCCESS) {
            computer->send_registered = false;
            computer->retry_pending = true;
            ++output_metrics[computer->slot].registration_errors;
            if (computer->last_send_error != status)
                printf("[output] PC%u send registration failed=0x%02x\n",
                       slot_of(computer) + 1, status);
            computer->last_send_error = status;
            break;
        }
        if (computer->send_registered || computer->sent == sent_before || !low_latency) break;
    }
    computer->pumping = false;
    if (low_latency && !budget && !computer->send_registered
        && !computer->retry_pending && !computer->motion_waiting
        && send_work(computer)) defer_pump(computer);
}

static void motion_timer_ready(void) {
    motion_timer_armed = false;
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) {
        computer_t *computer = &computers[i];
        if (!computer->motion_waiting) continue;
        computer->motion_waiting = false;
        if (mouse_buffering) request_send(computer);
    }
}

static void continue_pump(void *context) {
    (void)context;
    continuation_registered = false;
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) {
        computer_t *computer = &computers[i];
        bool pending = computer->continuation_pending;
        computer->continuation_pending = false;
        if ((low_latency || mouse_buffering) && pending) request_send(computer);
    }
    /* base_execute_callbacks drains until empty. Budget exhaustion MUST use a
       timer, not enqueue this callback again and monopolize the same run. */
}

static void pump_after_yield(btstack_timer_source_t *timer) {
    (void)timer;
    pump_timer_armed = false;
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) {
        computer_t *computer = &computers[i];
        bool pending = computer->yield_pending;
        computer->yield_pending = false;
        if (low_latency && pending) request_send(computer);
    }
}

void hub_output_set_low_latency(bool enabled) {
    low_latency = enabled;
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) {
        if (enabled) schedule_continuation(&computers[i]);
        else if (!mouse_buffering) computers[i].continuation_pending = false;
    }
}

bool hub_output_set_mouse_buffering(bool enabled) {
    mouse_buffering = enabled;
    if (!enabled) {
        for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) computers[i].motion_waiting = false;
        cancel_motion_timer();
    }
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) request_send(&computers[i]);
    hub_output_mouse_buffer_status();
    return mouse_buffering == enabled;
}

void hub_output_mouse_buffer_status(void) {
    printf("[mouse-buffer] enabled=%u reserve-free=%u hold-us=%u idle-ms=%u;"
           " RAM-only, boot default ON; shared ACL credits, NOT mouse ACKs\n",
           mouse_buffering, MOUSE_RESERVE_SLOTS, MOUSE_HOLD_US, MOUSE_IDLE_MS);
    for (unsigned slot = 0; slot < HUB_COMPUTER_COUNT; ++slot) {
        computer_t *computer = computer_for_slot(slot);
        const output_metrics_t *metrics = &output_metrics[slot];
        if (!computer && !metrics->motion_holds && !metrics->motion_deadline_sends) continue;
        printf("[mouse-buffer] PC%u waiting=%u free-acl=%d hold-entries=%" PRIu32
               " deadline-sends=%" PRIu32 "\n", slot + 1,
               computer && computer->motion_waiting,
               computer && computer->occupied
                   ? hci_number_free_acl_slots_for_handle(computer->handle) : -1,
               metrics->motion_holds, metrics->motion_deadline_sends);
    }
}

static bool queue_overflow(computer_t *computer, const char *reason) {
    ++computer->overflows;
    printf("[output] PC%u congestion/overflow (%s), discarded=%u; "
           "recovering to ALL RELEASED, input rejected\n",
           slot_of(computer) + 1, reason, computer->count);
    require_release(computer);
    active_readiness_lost(computer, "queue overflow");
    request_send(computer);
    return false;
}

bool hub_output_keyboard(const uint8_t report[8]) {
    computer_t *computer = computer_for_slot(active_slot);
    if (report == NULL || switch_pending || !hub_output_ready(active_slot)) return false;
    if (computer->count == OUTPUT_QUEUE_LENGTH) return queue_overflow(computer, "keyboard");
    unsigned tail = (computer->head + computer->count) % OUTPUT_QUEUE_LENGTH;
    output_event_t *event = &computer->queue[tail];
    event->kind = RELEASE_KEYBOARD;
    event->motion_only = event->first_motion = false;
    memcpy(event->value.keyboard, report, 8);
    event->value.keyboard[1] = 0;
    ++computer->count;
    measure_enqueue(computer, event, false);
    request_send(computer);
    return true;
}

bool hub_output_mouse(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel) {
    computer_t *computer = computer_for_slot(active_slot);
    if (switch_pending || !hub_output_ready(active_slot)) return false;
    buttons &= 0x1f;
    if (!hub_hid_motion_bounded(dx, dy, wheel)) return queue_overflow(computer, "excessive mouse delta");
    uint8_t previous_buttons = computer->last_mouse[0];
    for (unsigned i = computer->count; i > 0; --i) {
        const output_event_t *previous = &computer->queue[(computer->head + i - 1) % OUTPUT_QUEUE_LENGTH];
        if (previous->kind == RELEASE_MOUSE) {
            previous_buttons = previous->value.mouse.buttons;
            break;
        }
    }
    bool pure_motion = !wheel && (dx || dy) && buttons == previous_buttons;
    uint32_t now_ms = btstack_run_loop_get_time_ms();
    bool first_motion = pure_motion && !computer->count && (!computer->motion_seen
        || (uint32_t)(now_ms - computer->last_motion_ms) >= MOUSE_IDLE_MS);
    if (dx || dy) {
        computer->motion_seen = true;
        computer->last_motion_ms = now_ms;
    }
    if (computer->count != 0) {
        unsigned tail = (computer->head + computer->count - 1) % OUTPUT_QUEUE_LENGTH;
        output_event_t *event = &computer->queue[tail];
        if (event->kind == RELEASE_MOUSE && event->value.mouse.buttons == buttons
            && (!mouse_buffering || (pure_motion && event->motion_only))) {
            int64_t x = (int64_t)event->value.mouse.x + dx;
            int64_t y = (int64_t)event->value.mouse.y + dy;
            int64_t w = (int64_t)event->value.mouse.wheel + wheel;
            if (!hub_hid_motion_bounded(x, y, w)) return queue_overflow(computer, "mouse accumulation");
            event->value.mouse.x = (int32_t)x;
            event->value.mouse.y = (int32_t)y;
            event->value.mouse.wheel = (int32_t)w;
            event->motion_only = event->motion_only && pure_motion;
            measure_enqueue(computer, event, true);
            request_send(computer);
            return true;
        }
    }
    if (computer->count == OUTPUT_QUEUE_LENGTH) return queue_overflow(computer, "mouse buttons");
    unsigned tail = (computer->head + computer->count) % OUTPUT_QUEUE_LENGTH;
    output_event_t *event = &computer->queue[tail];
    event->kind = RELEASE_MOUSE;
    event->motion_only = pure_motion;
    event->first_motion = first_motion;
    event->value.mouse.buttons = buttons;
    event->value.mouse.x = dx;
    event->value.mouse.y = dy;
    event->value.mouse.wheel = wheel;
    ++computer->count;
    measure_enqueue(computer, event, false);
    request_send(computer);
    return true;
}

void hub_output_release_all(void) {
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) require_release(&computers[i]);
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) request_send(&computers[i]);
    progress_switch();
}

static void storage_fault(void) {
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) require_release(&computers[i]);
    hub_router_release_all();
}

bool hub_output_select(unsigned slot) {
    if (slot >= HUB_COMPUTER_COUNT || !hub_output_ready(slot)) {
        printf("[output] PC%u unavailable; selection stays PC%u\n", slot + 1, active_slot + 1);
        return false;
    }
    if (switch_pending) {
        printf("[output] switch already pending; request rejected\n");
        return false;
    }
    if (slot == active_slot) return true;
    /* Commit once, before acknowledging selection. A later timer only drains
       releases; it must neither rewrite flash nor choose another target. */
    if (!hub_output_store_select(&output_store, slot)) {
        storage_fault();
        printf("[output] selection rejected: persistent storage unavailable\n");
        return false;
    }
    switch_old_slot = active_slot;
    switch_target_slot = slot;
    computer_t *old = computer_for_slot(active_slot);
    switch_old_handle = old != NULL ? old->handle : HCI_CON_HANDLE_INVALID;
    switch_pending = true;
    switch_wait_log = btstack_run_loop_get_time_ms() + 5000;
    printf("[output] switching PC%u -> PC%u; discarding unsent old input, releasing old PC\n",
           active_slot + 1, slot + 1);
    clear_queue(computer_for_slot(slot));
    require_release(old);
    request_send(old);
    progress_switch();
    return true;
}

static uint8_t access_error(const computer_t *computer) {
    if (computer == NULL || computer->rejected || computer->disconnecting)
        return ATT_ERROR_INSUFFICIENT_AUTHORIZATION;
    uint8_t key_size = gap_encryption_key_size(computer->handle);
    if (key_size == 0) return ATT_ERROR_INSUFFICIENT_ENCRYPTION;
    if (key_size < 16) return ATT_ERROR_INSUFFICIENT_ENCRYPTION_KEY_SIZE;
    return 0;
}

static uint16_t read_attribute(hci_con_handle_t handle, uint16_t attribute, uint16_t offset,
                               uint8_t *buffer, uint16_t buffer_size) {
    if (attribute == ATT_CHARACTERISTIC_GAP_DEVICE_NAME_01_VALUE_HANDLE) {
        return att_read_callback_handle_blob((const uint8_t *)HUB_DEVICE_NAME,
            sizeof(HUB_DEVICE_NAME) - 1, offset, buffer, buffer_size);
    }
    computer_t *computer = computer_for_handle(handle);
    uint8_t error = access_error(computer);
    if (error != 0) return ATT_READ_ERROR_CODE_OFFSET + error;
    switch (attribute) {
        case ATT_CHARACTERISTIC_2A4B_01_VALUE_HANDLE:
            return att_read_callback_handle_blob(hub_hid_report_descriptor, sizeof(hub_hid_report_descriptor),
                                                offset, buffer, buffer_size);
        case KEYBOARD_CCCD:
            return att_read_callback_handle_little_endian_16(computer->keyboard_cccd,
                                                            offset, buffer, buffer_size);
        case MOUSE_CCCD:
            return att_read_callback_handle_little_endian_16(computer->mouse_cccd,
                                                            offset, buffer, buffer_size);
        case LED_HANDLE:
            return att_read_callback_handle_byte(computer->leds, offset, buffer, buffer_size);
        case KEYBOARD_HANDLE: {
            const uint8_t neutral[8] = {0};
            const uint8_t *value = computer->release_pending || !transport_ready(computer)
                ? neutral : computer->last_keyboard;
            return att_read_callback_handle_blob(value, 8, offset, buffer, buffer_size);
        }
        case MOUSE_HANDLE: {
            /* Relative movement must never be replayed by an ATT read. */
            uint8_t value[6] = {0};
            if (!computer->release_pending && transport_ready(computer))
                value[0] = computer->last_mouse[0];
            return att_read_callback_handle_blob(value, 6, offset, buffer, buffer_size);
        }
        default:
            return 0;
    }
}

static int write_attribute(hci_con_handle_t handle, uint16_t attribute, uint16_t transaction,
                            uint16_t offset, uint8_t *buffer, uint16_t size) {
    if (transaction != ATT_TRANSACTION_MODE_NONE) return ATT_ERROR_REQUEST_NOT_SUPPORTED;
    computer_t *computer = computer_for_handle(handle);
    uint8_t error = access_error(computer);
    if (error != 0) return error;
    if (offset != 0) return ATT_ERROR_INVALID_OFFSET;
    bool was_ready = transport_ready(computer);
    if (attribute == KEYBOARD_CCCD || attribute == MOUSE_CCCD) {
        if (size != 2) return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
        uint16_t value = little_endian_read_16(buffer, 0);
        if (value > 1) return ATT_ERROR_VALUE_NOT_ALLOWED;
        if (attribute == KEYBOARD_CCCD) computer->keyboard_cccd = value;
        else computer->mouse_cccd = value;
        printf("[output] handle=0x%04x %s notifications=%u (identity-bound=%u)\n",
               handle, attribute == KEYBOARD_CCCD ? "keyboard" : "mouse", value,
               computer->identity_authenticated);
    } else if (attribute == LED_HANDLE) {
        if (size != 1) return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
        computer->leds = buffer[0] & 0x1f;
        printf("[output] handle=0x%04x keyboard LEDs=0x%02x\n", handle, computer->leds);
    } else if (attribute == ATT_CHARACTERISTIC_2A4C_01_VALUE_HANDLE) {
        if (size != 1) return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
        if (buffer[0] > 1) return ATT_ERROR_VALUE_NOT_ALLOWED;
        computer->suspended = buffer[0] == 0;
        printf("[output] handle=0x%04x suspend=%u\n", handle, computer->suspended);
    } else {
        return ATT_ERROR_WRITE_NOT_PERMITTED;
    }
    if (was_ready && !transport_ready(computer)) {
        require_release(computer);
        active_readiness_lost(computer, "lost notifications or entered suspend");
    }
    request_send(computer);
    progress_switch();
    log_readiness(computer);
    return 0;
}

static void update_advertising(void) {
    if (!started) return;
    unsigned occupied = 0;
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) occupied += computers[i].occupied;
    advertising_requested = occupied < HUB_COMPUTER_COUNT;
    gap_advertisements_enable(advertising_requested);
    printf("[output] legacy connectable advertising requested=%u occupied=%u/%u "
           "(controller capacity unproven)\n",
           advertising_requested, occupied, HUB_COMPUTER_COUNT);
}

static void connected(uint8_t *packet) {
    if (gap_subevent_le_connection_complete_get_role(packet) != HCI_ROLE_SLAVE) return;
    uint8_t status = gap_subevent_le_connection_complete_get_status(packet);
    if (status != ERROR_CODE_SUCCESS) {
        printf("[output] LE connection completion failed status=0x%02x\n", status);
        update_advertising();
        return;
    }
    hci_con_handle_t handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
    if (computer_for_handle(handle) != NULL) return;
    computer_t *computer = NULL;
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) {
        if (!computers[i].occupied && !computers[i].retiring) {
            computer = &computers[i];
            break;
        }
    }
    if (computer == NULL) {
        printf("[output] excess LE-peripheral handle=0x%04x rejected: connection contexts full\n",
               handle);
        gap_disconnect(handle);
        update_advertising();
        return;
    }
    memset(computer, 0, sizeof(*computer));
    computer->occupied = true;
    computer->slot = HUB_COMPUTER_COUNT;
    computer->handle = handle;
    computer->release_pending = RELEASE_BOTH;
    computer->address_type = gap_subevent_le_connection_complete_get_peer_address_type(packet);
    gap_subevent_le_connection_complete_get_peer_address(packet, computer->address);
    computer->auth_deadline = btstack_run_loop_get_time_ms() + OUTPUT_AUTH_TIMEOUT_MS;
    computer->send_registration.callback = can_send;
    computer->send_registration.context = computer;
    printf("[output] provisional connected address=%s type=%u role=LE-peripheral handle=0x%04x "
           "pairing-window=%u gate=%u; requesting security, NOT yet ready\n",
           bd_addr_to_str(computer->address), computer->address_type,
           handle, hub_security_pairing_open(), hub_security_allow_pairing(handle));
    sm_request_pairing(handle);
    update_advertising();
}

static void reject_computer(computer_t *computer, const char *reason) {
    printf("[output] handle=0x%04x rejected: %s; saved slots unchanged\n",
           computer->handle, reason);
    computer->rejected = true;
    computer->identity_authenticated = false;
    require_release(computer);
    active_readiness_lost(computer, reason);
    uint8_t status = gap_disconnect(computer->handle);
    computer->disconnecting = status == ERROR_CODE_SUCCESS;
    computer->auth_deadline = btstack_run_loop_get_time_ms() + 1000;
    if (status) printf("[output] rejection disconnect failed=0x%02x; retry pending\n", status);
}

static bool authenticated_identity_unchanged(const computer_t *computer) {
    if (!secure(computer) || !computer->authentication_complete) return false;
    int index = sm_le_device_index(computer->handle);
    if (index < 0 || index >= le_device_db_max_count()) return false;
    int type = -1, key_size = 0;
    bd_addr_t identity = {0};
    le_device_db_info(index, &type, identity, NULL);
    le_device_db_encryption_get(index, NULL, NULL, NULL, &key_size, NULL, NULL, NULL);
    return type >= 0 && type <= 1 && key_size >= 16
        && hub_output_store_find(&output_store, (uint8_t)type, identity) == (int)computer->slot;
}

static void bind_identity(computer_t *computer) {
    if (computer->identity_authenticated || computer->rejected || computer->disconnecting
        || !computer->authentication_complete || computer->key_size < 16
        || !output_store.loaded || output_store.fault) return;
    int index = sm_le_device_index(computer->handle);
    if (index < 0 || index >= le_device_db_max_count()) return;
    int type = -1, bond_key_size = 0;
    bd_addr_t identity = {0};
    le_device_db_info(index, &type, identity, NULL);
    /* Query only this peripheral link's resolved bond, never enumerate the DB
       shared with central inputs. No keys are copied or logged. Just Works bonds
       may be non-MITM: authentication here means proof of their 128-bit LTK. */
    le_device_db_encryption_get(index, NULL, NULL, NULL, &bond_key_size, NULL, NULL, NULL);
    if (bond_key_size < 16 || type < 0 || type > 1
        || !hub_output_identity_valid((uint8_t)type, identity)) return;
    int slot = hub_output_store_find(&output_store, (uint8_t)type, identity);
    if (computer->slot < HUB_COMPUTER_COUNT && slot != (int)computer->slot) {
        reject_computer(computer, "bond identity changed within connection");
        return;
    }
    if (slot >= 0 && slot_connections[slot] != NULL && slot_connections[slot] != computer) {
        reject_computer(computer, "duplicate connected computer identity");
        return;
    }
    if (slot < 0) {
        slot = hub_output_store_claim(&output_store, (uint8_t)type, identity);
        if (slot < 0) {
            if (output_store.fault) storage_fault();
            reject_computer(computer, output_store.fault ? "identity persistence failed"
                                                        : "all four identities reserved");
            return;
        }
    }
    computer->slot = (unsigned)slot;
    slot_connections[slot] = computer;
    computer->identity_authenticated = true;
    require_release(computer);
    printf("[output] PC%u bound handle=0x%04x identity=%s type=%d bond-index=%d "
           "selected=%u; connection CCCDs retained, neutral reports required\n",
           computer->slot + 1, computer->handle, bd_addr_to_str(identity), type, index,
           computer->slot == active_slot);
}

static void refresh_security(computer_t *computer) {
    uint8_t key_size = gap_encryption_key_size(computer->handle);
    if (key_size == computer->key_size) {
        /* Identity resolution and bond storage may finish after encryption and
           after ATT restores/writes CCCDs. Preserve those per-link writes. */
        bind_identity(computer);
        return;
    }
    printf("[output] handle=0x%04x encryption key-size=%u\n", computer->handle, key_size);
    bool lost_security = computer->key_size >= 16 && key_size < 16;
    computer->key_size = key_size;
    if (lost_security) {
        computer->authentication_complete = false;
        computer->identity_authenticated = false;
        require_release(computer);
        computer->auth_deadline = btstack_run_loop_get_time_ms() + OUTPUT_AUTH_TIMEOUT_MS;
        active_readiness_lost(computer, "lost encryption");
    }
    bind_identity(computer);
    progress_switch();
    log_readiness(computer);
}

static void hci_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type != HCI_EVENT_PACKET) return;
    computer_t *computer;
    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_NUMBER_OF_COMPLETED_PACKETS:
            /* Recheck after all HCI handlers, without a synchronous ATT spin. */
            if (mouse_buffering) {
                for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i)
                    if (computers[i].motion_waiting) schedule_continuation(&computers[i]);
            }
            break;
        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE)
                connected(packet);
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            computer = computer_for_handle(hci_event_disconnection_complete_get_connection_handle(packet));
            if (computer == NULL) break;
            printf("[output] handle=0x%04x disconnected reason=0x%02x address=%s "
                   "logical-PC=%u (0=provisional) release-pending=0x%02x\n", computer->handle,
                   hci_event_disconnection_complete_get_reason(packet),
                   bd_addr_to_str(computer->address),
                   computer->slot < HUB_COMPUTER_COUNT ? computer->slot + 1 : 0,
                   computer->release_pending);
            computer->occupied = false;
            computer->identity_authenticated = false;
            /* The SDK disconnect handler purges ATT notification_requests.
               Quarantine until the next run-loop timer, after ALL HCI handlers
               have run, before reusing this callback's storage for a new link. */
            computer->retiring = true;
            computer->continuation_pending = false;
            computer->yield_pending = false;
            computer->retry_pending = false;
            if (computer_for_slot(computer->slot) == computer)
                slot_connections[computer->slot] = NULL;
            clear_queue(computer);
            computer->release_pending = 0;
            memset(computer->last_keyboard, 0, sizeof(computer->last_keyboard));
            memset(computer->last_mouse, 0, sizeof(computer->last_mouse));
            active_readiness_lost(computer, "disconnected");
            progress_switch();
            update_advertising();
            break;
        case HCI_EVENT_ENCRYPTION_CHANGE:
        case HCI_EVENT_ENCRYPTION_CHANGE_V2:
        case HCI_EVENT_ENCRYPTION_KEY_REFRESH_COMPLETE:
            computer = computer_for_handle(little_endian_read_16(packet, 3));
            if (computer != NULL) refresh_security(computer);
            break;
        case HCI_EVENT_COMMAND_COMPLETE:
            if (size >= 6) {
                uint16_t opcode = hci_event_command_complete_get_command_opcode(packet);
                if (opcode >= 0x2006 && opcode <= 0x200a) {
                    advertising_status = packet[5];
                    if (advertising_status != 0 || opcode == 0x200a)
                        printf("[output] legacy advertising HCI opcode=0x%04x status=0x%02x "
                               "requested=%u\n", opcode, advertising_status, advertising_requested);
                }
            }
            break;
        default:
            break;
    }
}

static void security_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    hci_con_handle_t handle;
    uint8_t status;
    const char *operation;
    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_PAIRING_STARTED:
        case SM_EVENT_REENCRYPTION_STARTED: {
            computer_t *computer = computer_for_handle(little_endian_read_16(packet, 2));
            if (computer == NULL) return;
            printf("[output] security-start event=0x%02x handle=0x%04x key=%u identity-bound=%u\n",
                   hci_event_packet_get_type(packet), computer->handle,
                   gap_encryption_key_size(computer->handle), computer->identity_authenticated);
            /* BTstack may announce another security request after this very link
               has already completed re-encryption. That notification alone does
               not revoke its established encrypted identity or guarantee a new
               completion event. Real pairing, encryption loss and failure still
               invalidate readiness through their respective handlers. */
            if (hci_event_packet_get_type(packet) == SM_EVENT_REENCRYPTION_STARTED
                && authenticated_identity_unchanged(computer)) {
                printf("[output] PC%u retains verified encrypted identity during redundant re-encryption request\n",
                       computer->slot + 1);
                return;
            }
            bool was_authenticated = computer->identity_authenticated;
            computer->authentication_complete = false;
            computer->identity_authenticated = false;
            require_release(computer);
            if (was_authenticated) active_readiness_lost(computer, "security setup restarted");
            return;
        }
        case SM_EVENT_PAIRING_COMPLETE:
            handle = sm_event_pairing_complete_get_handle(packet);
            status = sm_event_pairing_complete_get_status(packet);
            operation = "pairing";
            break;
        case SM_EVENT_REENCRYPTION_COMPLETE:
            handle = sm_event_reencryption_complete_get_handle(packet);
            status = sm_event_reencryption_complete_get_status(packet);
            operation = "reencryption";
            break;
        default:
            return; /* Pairing confirmations belong exclusively to main. */
    }
    computer_t *computer = computer_for_handle(handle);
    if (computer == NULL) return;
    printf("[output] handle=0x%04x %s status=0x%02x\n", handle, operation, status);
    if (status != ERROR_CODE_SUCCESS) {
        /* BTstack emits this before continuing a new Pairing Request when only
           our side retains an LTK. Never treat it as authentication success. */
        if (hci_event_packet_get_type(packet) == SM_EVENT_REENCRYPTION_COMPLETE
            && status == ERROR_CODE_PIN_OR_KEY_MISSING
            && hub_security_pairing_open() && hub_security_allow_pairing(handle)
            && computer->slot == HUB_COMPUTER_COUNT && !computer->rejected
            && !computer->disconnecting && !computer->authentication_complete
            && !computer->identity_authenticated && gap_encryption_key_size(handle) == 0) {
            require_release(computer);
            printf("[output] peer lacks old BLE bond; explicit pairing window permits"
                   " fresh pairing; still unauthenticated, original deadline retained\n");
            return;
        }
        reject_computer(computer, "pairing/reencryption failed");
        return;
    }
    computer->authentication_complete = true;
    refresh_security(computer);
}

static void service(btstack_timer_source_t *timer) {
    uint32_t now = btstack_run_loop_get_time_ms();
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) {
        computer_t *computer = &computers[i];
        if (!computer->occupied) {
            computer->retiring = false;
            computer->send_registered = false;
            continue;
        }
        refresh_security(computer);
        if (!computer->disconnecting && !secure(computer)
            && (int32_t)(now - computer->auth_deadline) >= 0) {
            printf("[output] handle=0x%04x SECURITY TIMEOUT: disconnecting "
                   "unbound/unauthenticated/short-key link, not an inactive ready PC\n",
                   computer->handle);
            require_release(computer);
            uint8_t status = gap_disconnect(computer->handle);
            computer->disconnecting = status == ERROR_CODE_SUCCESS;
            if (status != 0) {
                printf("[output] security-timeout disconnect failed=0x%02x\n", status);
                computer->auth_deadline = now + 1000;
            }
        }
        computer->retry_pending = false;
        request_send(computer);
        log_readiness(computer);
    }
    progress_switch();
    if (switch_pending && (int32_t)(now - switch_wait_log) >= 0) {
        printf("[output] switch waiting for PC%u neutral reports; input suppressed\n",
               switch_old_slot + 1);
        switch_wait_log = now + 5000;
    }
    btstack_run_loop_set_timer(timer, OUTPUT_SERVICE_INTERVAL_MS);
    btstack_run_loop_add_timer(timer);
}

void hub_output_init(void) {
    _Static_assert(HUB_COMPUTER_COUNT == 4, "This feasibility firmware has four PC slots");
    _Static_assert(sizeof(HUB_DEVICE_NAME) - 1 <= 29, "BLE scan-response name too long");
    low_latency = continuation_registered = pump_timer_armed = false;
    mouse_buffering = true;
    cancel_motion_timer();
    memset(output_metrics, 0, sizeof(output_metrics));
    pump_registration.callback = continue_pump;
    pump_registration.context = NULL;
    btstack_run_loop_set_timer_handler(&pump_timer, pump_after_yield);
    /* BTstack otherwise stops legacy connectable advertising after one link. */
    gap_set_max_number_peripheral_connections(HUB_COMPUTER_COUNT);
    att_server_init(profile_data, read_attribute, write_attribute);
    hci_registration.callback = hci_event;
    hci_add_event_handler(&hci_registration);
    sm_registration.callback = security_event;
    sm_add_event_handler(&sm_registration);
    btstack_run_loop_set_timer_handler(&service_timer, service);
    printf("[output] custom per-peer HOGP initialized: keyboard ID1=8 bytes, mouse ID2=6 bytes; "
           "four simultaneous PCs are a hardware test goal, not a verified capability\n");
}

void hub_output_start(void) {
    if (started) return;
    started = true;
    /* The port installs TLV only once the controller reaches WORKING. */
    hub_output_store_load(&output_store);
    active_slot = output_store.selected;
    static uint8_t advertising_data[] = {
        2, BLUETOOTH_DATA_TYPE_FLAGS, 0x02,
        3, BLUETOOTH_DATA_TYPE_COMPLETE_LIST_OF_16_BIT_SERVICE_CLASS_UUIDS, 0x12, 0x18,
        3, BLUETOOTH_DATA_TYPE_APPEARANCE, 0xc0, 0x03
    };
    static uint8_t scan_response[sizeof(HUB_DEVICE_NAME) + 1];
    scan_response[0] = sizeof(HUB_DEVICE_NAME);
    scan_response[1] = BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME;
    memcpy(&scan_response[2], HUB_DEVICE_NAME, sizeof(HUB_DEVICE_NAME) - 1);
    bd_addr_t direct_address = {0};
    gap_advertisements_set_params(0x00a0, 0x00f0, 0, 0, direct_address, 0x07, 0);
    gap_advertisements_set_data(sizeof(advertising_data), advertising_data);
    gap_scan_response_set_data(sizeof(scan_response), scan_response);
    update_advertising();
    btstack_run_loop_set_timer(&service_timer, OUTPUT_SERVICE_INTERVAL_MS);
    btstack_run_loop_add_timer(&service_timer);
}

void hub_output_latency_reset(void) {
    memset(output_metrics, 0, sizeof(output_metrics));
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) {
        computer_t *computer = &computers[i];
        for (unsigned n = 0; n < computer->count; ++n)
            computer->queue[(computer->head + n) % OUTPUT_QUEUE_LENGTH].timing_valid = false;
        if (!computer->occupied || computer->slot >= HUB_COMPUTER_COUNT) continue;
        output_metrics_t *metrics = &output_metrics[computer->slot];
        metrics->queue_highwater = computer->count;
        for (unsigned kind = 0; kind < 2; ++kind)
            metrics->reports[kind].queue_highwater = computer->queued[kind];
    }
    printf("[latency output] reset; queues/connections unchanged, current depths seed peaks; "
           "pre-reset queued events excluded from timing (including later merges)\n");
}

void hub_output_latency_status(void) {
    hub_output_mouse_buffer_status();
    printf("[latency output] mode=%s service-fallback=%ums burst-budget=%u yield-timer=%ums\n",
           low_latency ? "fast" : "baseline", OUTPUT_SERVICE_INTERVAL_MS,
           OUTPUT_PUMP_BUDGET, OUTPUT_PUMP_YIELD_MS);
    printf("[latency output] MCU queue-to-successful-ATT-notify us, NOT radio/end-to-end; "
           "per-logical-PC since boot/reset across reconnects; enqueue=accepted calls incl merges; "
           "notify=ordinary packets incl split motion; timing=one sample/notify, oldest enqueue "
           "retained for merges/splits, excludes pre-reset events and safety releases\n");
    printf("[latency output] wait-bins=<1ms/1..<5ms/5..<10ms/10..<20ms/>=20ms; "
           "32-bit clock subtraction (queue age must be <~71min)\n");
    for (unsigned slot = 0; slot < HUB_COMPUTER_COUNT; ++slot) {
        const output_metrics_t *metrics = &output_metrics[slot];
        const computer_t *computer = computer_for_slot(slot);
        printf("[latency output] PC%u queued=%u peak=%u mouse-merges=%" PRIu32
               " safety-releases=%" PRIu32 " release-errors=%" PRIu32
               " registration-errors=%" PRIu32 " budget-yields=%" PRIu32 "\n",
               slot + 1, computer ? computer->count : 0, metrics->queue_highwater,
               metrics->mouse_merges, metrics->releases, metrics->release_errors,
               metrics->registration_errors, metrics->budget_yields);
        for (unsigned kind = 0; kind < 2; ++kind) {
            const output_report_metrics_t *report = &metrics->reports[kind];
            printf("[latency output] PC%u %s enqueue=%" PRIu32 " notify=%" PRIu32
                   " queued=%u peak=%u samples=%" PRIu32 " sum-us=%" PRIu64
                   " avg-us=%" PRIu64 " max-us=%" PRIu32
                   " bins=%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32
                   " notify-errors=%" PRIu32 "\n",
                   slot + 1, kind == 0 ? "keyboard" : "mouse",
                   report->enqueued, report->notified, computer ? computer->queued[kind] : 0,
                   report->queue_highwater, report->wait_count, report->wait_sum_us,
                   report->wait_count ? report->wait_sum_us / report->wait_count : UINT64_C(0),
                   report->wait_max_us, report->wait_bins[0], report->wait_bins[1],
                   report->wait_bins[2], report->wait_bins[3], report->wait_bins[4],
                   report->notify_errors);
        }
    }
}

void hub_output_status(void) {
    hub_output_mouse_buffer_status();
    printf("[output] selected=PC%u saved-selected=PC%u storage=%s switching=%u "
           "advertising-requested=%u last-adv-status=0x%02x\n",
           active_slot + 1, output_store.selected + 1,
           output_store.fault ? "LOCKED" : output_store.loaded ? "verified" : "not-loaded",
           switch_pending, advertising_requested, advertising_status);
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) {
        const hub_output_identity_t *identity = &output_store.identities[i];
        computer_t *computer = computer_for_slot(i);
        if (!identity->reserved) {
            printf("[output] PC%u %s%s\n", i + 1,
                   output_store.fault ? "UNKNOWN/LOCKED" : "vacant",
                   active_slot == i ? " SELECTED" : "");
            continue;
        }
        printf("[output] PC%u%s reserved identity=%s type=%u %s\n",
               i + 1, active_slot == i ? " SELECTED" : "",
               bd_addr_to_str(identity->address), identity->type, computer ? "online" : "OFFLINE");
        if (computer == NULL) {
            continue;
        }
        printf("[output] PC%u%s address=%s type=%u role=LE-peripheral handle=0x%04x "
               "ready=%u key=%u notify(k/m)=%u/%u suspend=%u LEDs=0x%02x "
               "queued=%u release=0x%02x sent=%" PRIu32 " overflow=%" PRIu32 "\n",
               i + 1, active_slot == i ? " SELECTED" : "", bd_addr_to_str(computer->address),
               computer->address_type, computer->handle, hub_output_ready(i),
               gap_encryption_key_size(computer->handle), computer->keyboard_cccd,
               computer->mouse_cccd, computer->suspended, computer->leds, computer->count,
               computer->release_pending, computer->sent, computer->overflows);
        const hci_connection_t *connection = hci_connection_for_handle(computer->handle);
        printf("[output] PC%u auth-complete=%u identity-bound=%u send-pending=%u last-send=0x%02x "
               "SM-state=%u SM-pairing=%u SM-reencrypt=%u\n",
               i + 1, computer->authentication_complete, computer->identity_authenticated,
               computer->send_registered, computer->last_send_error,
               connection ? (unsigned)connection->sm_connection.sm_engine_state : 0,
               connection ? connection->sm_connection.sm_pairing_active : 0,
               connection ? connection->sm_connection.sm_reencryption_active : 0);
    }
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) {
        const computer_t *computer = &computers[i];
        if (!computer->occupied || computer->slot < HUB_COMPUTER_COUNT) continue;
        printf("[output] provisional handle=0x%04x address=%s type=%u key=%u "
               "notify(k/m)=%u/%u rejected=%u; no logical PC slot, no routing\n",
               computer->handle, bd_addr_to_str(computer->address), computer->address_type,
               computer->key_size, computer->keyboard_cccd, computer->mouse_cccd,
               computer->rejected);
    }
}
