#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "btstack.h"
#include "hub_config.h"
#include "hub_output.h"
#include "hub_router.h"
#include "hub_security.h"
#include "hub.h"

#define OUTPUT_QUEUE_LENGTH 64u
#define OUTPUT_AUTH_TIMEOUT_MS 90000u
#define OUTPUT_SERVICE_INTERVAL_MS 5u
#define RELEASE_KEYBOARD 1u
#define RELEASE_MOUSE 2u
#define RELEASE_BOTH (RELEASE_KEYBOARD | RELEASE_MOUSE)
#define KEYBOARD_HANDLE ATT_CHARACTERISTIC_2A4D_01_VALUE_HANDLE
#define KEYBOARD_CCCD ATT_CHARACTERISTIC_2A4D_01_CLIENT_CONFIGURATION_HANDLE
#define LED_HANDLE ATT_CHARACTERISTIC_2A4D_02_VALUE_HANDLE
#define MOUSE_HANDLE ATT_CHARACTERISTIC_2A4D_03_VALUE_HANDLE
#define MOUSE_CCCD ATT_CHARACTERISTIC_2A4D_03_CLIENT_CONFIGURATION_HANDLE

/* IDs identify GATT report references; IDs are not part of report payloads. */
static const uint8_t report_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
    0x75, 0x01, 0x95, 0x05, 0x91, 0x02,
    0x75, 0x03, 0x95, 0x01, 0x91, 0x03,
    0x05, 0x07, 0x19, 0x00, 0x29, 0xff, 0x15, 0x00, 0x26, 0xff, 0x00,
    0x75, 0x08, 0x95, 0x06, 0x81, 0x00, 0xc0,
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02,
    0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x05, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x05, 0x81, 0x02,
    0x75, 0x03, 0x95, 0x01, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x16, 0x00, 0x80, 0x26, 0xff, 0x7f,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x06,
    0x09, 0x38, 0x15, 0x80, 0x25, 0x7f,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0
};

typedef struct {
    uint8_t kind;
    union {
        uint8_t keyboard[8];
        struct {
            uint8_t buttons;
            int32_t x, y, wheel;
        } mouse;
    } value;
} output_event_t;

typedef struct {
    bool occupied;
    bool disconnecting;
    bool suspended;
    bool send_registered;
    bool ready_logged;
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
    btstack_context_callback_registration_t send_registration;
} computer_t;

static computer_t computers[HUB_COMPUTER_COUNT];
static btstack_packet_callback_registration_t hci_registration;
static btstack_packet_callback_registration_t sm_registration;
static btstack_timer_source_t service_timer;
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

static unsigned slot_of(const computer_t *computer) {
    return (unsigned)(computer - computers);
}

static computer_t *computer_for_handle(hci_con_handle_t handle) {
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) {
        if (computers[i].occupied && computers[i].handle == handle) return &computers[i];
    }
    return NULL;
}

static bool secure(const computer_t *computer) {
    return computer->occupied && !computer->disconnecting
        && gap_encryption_key_size(computer->handle) >= 16;
}

static bool transport_ready(const computer_t *computer) {
    return secure(computer) && !computer->suspended
        && computer->keyboard_cccd == 1 && computer->mouse_cccd == 1;
}

bool hub_output_ready(unsigned slot) {
    return slot < HUB_COMPUTER_COUNT && transport_ready(&computers[slot])
        && computers[slot].release_pending == 0;
}

unsigned hub_output_active_slot(void) {
    return active_slot;
}

bool hub_output_switching(void) {
    return switch_pending;
}

static void clear_queue(computer_t *computer) {
    memset(computer->queue, 0, sizeof(computer->queue));
    computer->head = 0;
    computer->count = 0;
}

static void require_release(computer_t *computer) {
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
    computer_t *old = &computers[switch_old_slot];
    bool same_link = old->occupied && old->handle == switch_old_handle;
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
    memset(&computer->queue[computer->head], 0, sizeof(output_event_t));
    computer->head = (computer->head + 1) % OUTPUT_QUEUE_LENGTH;
    --computer->count;
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
    uint8_t status = att_server_notify(computer->handle,
        kind == RELEASE_KEYBOARD ? KEYBOARD_HANDLE : MOUSE_HANDLE,
        report, kind == RELEASE_KEYBOARD ? 8 : 6);
    if (status != ERROR_CODE_SUCCESS) {
        if (computer->last_send_error != status) {
            printf("[output] PC%u handle=0x%04x notify failed=0x%02x; retry pending\n",
                   slot_of(computer) + 1, computer->handle, status);
        }
        computer->last_send_error = status;
        return;
    }
    computer->last_send_error = 0;
    ++computer->sent;
    if (kind == RELEASE_KEYBOARD) memcpy(computer->last_keyboard, report, 8);
    else memcpy(computer->last_mouse, report, 6);
    if (release) {
        computer->release_pending &= (uint8_t)~kind;
    } else if (kind == RELEASE_KEYBOARD) {
        pop_event(computer);
    } else {
        event->value.mouse.x -= dx;
        event->value.mouse.y -= dy;
        event->value.mouse.wheel -= wheel;
        if (event->value.mouse.x == 0 && event->value.mouse.y == 0
            && event->value.mouse.wheel == 0) pop_event(computer);
    }
    progress_switch();
    log_readiness(computer);
    /* Registration may invoke us synchronously. The service timer pumps further
       queued reports instead of recursively registering from this callback. */
}

static void request_send(computer_t *computer) {
    if (!computer->occupied || computer->send_registered) return;
    bool input_pending = !switch_pending && slot_of(computer) == active_slot
        && computer->count != 0 && hub_output_ready(slot_of(computer));
    if (!sendable_release(computer) && !input_pending) return;
    computer->send_registered = true;
    uint8_t status = att_server_request_to_send_notification(
        &computer->send_registration, computer->handle);
    if (status != ERROR_CODE_SUCCESS) {
        computer->send_registered = false;
        if (computer->last_send_error != status)
            printf("[output] PC%u send registration failed=0x%02x\n",
                   slot_of(computer) + 1, status);
        computer->last_send_error = status;
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
    computer_t *computer = &computers[active_slot];
    if (report == NULL || switch_pending || !hub_output_ready(active_slot)) return false;
    if (computer->count == OUTPUT_QUEUE_LENGTH) return queue_overflow(computer, "keyboard");
    unsigned tail = (computer->head + computer->count) % OUTPUT_QUEUE_LENGTH;
    output_event_t *event = &computer->queue[tail];
    event->kind = RELEASE_KEYBOARD;
    memcpy(event->value.keyboard, report, 8);
    event->value.keyboard[1] = 0;
    ++computer->count;
    request_send(computer);
    return true;
}

static bool bounded_motion(int64_t x, int64_t y, int64_t wheel) {
    /* Bound work as well as RAM: enormous deltas must not delay a button-up for
       millions of notifications. Intermediate arithmetic cannot overflow int32. */
    return x >= -32768 * 32 && x <= 32767 * 32
        && y >= -32768 * 32 && y <= 32767 * 32
        && wheel >= -128 * 32 && wheel <= 127 * 32;
}

bool hub_output_mouse(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel) {
    computer_t *computer = &computers[active_slot];
    if (switch_pending || !hub_output_ready(active_slot)) return false;
    buttons &= 0x1f;
    if (!bounded_motion(dx, dy, wheel)) return queue_overflow(computer, "excessive mouse delta");
    if (computer->count != 0) {
        unsigned tail = (computer->head + computer->count - 1) % OUTPUT_QUEUE_LENGTH;
        output_event_t *event = &computer->queue[tail];
        if (event->kind == RELEASE_MOUSE && event->value.mouse.buttons == buttons) {
            int64_t x = (int64_t)event->value.mouse.x + dx;
            int64_t y = (int64_t)event->value.mouse.y + dy;
            int64_t w = (int64_t)event->value.mouse.wheel + wheel;
            if (!bounded_motion(x, y, w)) return queue_overflow(computer, "mouse accumulation");
            event->value.mouse.x = (int32_t)x;
            event->value.mouse.y = (int32_t)y;
            event->value.mouse.wheel = (int32_t)w;
            request_send(computer);
            return true;
        }
    }
    if (computer->count == OUTPUT_QUEUE_LENGTH) return queue_overflow(computer, "mouse buttons");
    unsigned tail = (computer->head + computer->count) % OUTPUT_QUEUE_LENGTH;
    output_event_t *event = &computer->queue[tail];
    event->kind = RELEASE_MOUSE;
    event->value.mouse.buttons = buttons;
    event->value.mouse.x = dx;
    event->value.mouse.y = dy;
    event->value.mouse.wheel = wheel;
    ++computer->count;
    request_send(computer);
    return true;
}

void hub_output_release_all(void) {
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) require_release(&computers[i]);
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) request_send(&computers[i]);
    progress_switch();
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
    switch_old_slot = active_slot;
    switch_target_slot = slot;
    computer_t *old = &computers[active_slot];
    switch_old_handle = old->occupied ? old->handle : HCI_CON_HANDLE_INVALID;
    switch_pending = true;
    switch_wait_log = btstack_run_loop_get_time_ms() + 5000;
    printf("[output] switching PC%u -> PC%u; discarding unsent old input, releasing old PC\n",
           active_slot + 1, slot + 1);
    clear_queue(&computers[slot]);
    require_release(old);
    request_send(old);
    progress_switch();
    return true;
}

static uint8_t access_error(const computer_t *computer) {
    if (computer == NULL) return ATT_ERROR_INSUFFICIENT_AUTHORIZATION;
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
            return att_read_callback_handle_blob(report_map, sizeof(report_map),
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
        printf("[output] PC%u handle=0x%04x %s notifications=%u\n",
               slot_of(computer) + 1, handle, attribute == KEYBOARD_CCCD ? "keyboard" : "mouse", value);
    } else if (attribute == LED_HANDLE) {
        if (size != 1) return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
        computer->leds = buffer[0] & 0x1f;
        printf("[output] PC%u keyboard LEDs=0x%02x\n", slot_of(computer) + 1, computer->leds);
    } else if (attribute == ATT_CHARACTERISTIC_2A4C_01_VALUE_HANDLE) {
        if (size != 1) return ATT_ERROR_INVALID_ATTRIBUTE_VALUE_LENGTH;
        if (buffer[0] > 1) return ATT_ERROR_VALUE_NOT_ALLOWED;
        computer->suspended = buffer[0] == 0;
        printf("[output] PC%u suspend=%u\n", slot_of(computer) + 1, computer->suspended);
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
        if (!computers[i].occupied) {
            computer = &computers[i];
            break;
        }
    }
    if (computer == NULL) {
        printf("[output] excess LE-peripheral handle=0x%04x rejected: four PC slots occupied\n",
               handle);
        gap_disconnect(handle);
        update_advertising();
        return;
    }
    memset(computer, 0, sizeof(*computer));
    computer->occupied = true;
    computer->handle = handle;
    computer->address_type = gap_subevent_le_connection_complete_get_peer_address_type(packet);
    gap_subevent_le_connection_complete_get_peer_address(packet, computer->address);
    computer->auth_deadline = btstack_run_loop_get_time_ms() + OUTPUT_AUTH_TIMEOUT_MS;
    computer->send_registration.callback = can_send;
    computer->send_registration.context = computer;
    printf("[output] PC%u connected address=%s type=%u role=LE-peripheral handle=0x%04x "
           "pairing-window=%u gate=%u; requesting security, NOT yet ready\n",
           slot_of(computer) + 1, bd_addr_to_str(computer->address), computer->address_type,
           handle, hub_security_pairing_open(), hub_security_allow_pairing(handle));
    sm_request_pairing(handle);
    update_advertising();
}

static void refresh_security(computer_t *computer) {
    uint8_t key_size = gap_encryption_key_size(computer->handle);
    if (key_size == computer->key_size) return;
    printf("[output] PC%u handle=0x%04x encryption key-size=%u\n",
           slot_of(computer) + 1, computer->handle, key_size);
    bool lost_security = computer->key_size >= 16 && key_size < 16;
    computer->key_size = key_size;
    if (lost_security) {
        require_release(computer);
        computer->auth_deadline = btstack_run_loop_get_time_ms() + OUTPUT_AUTH_TIMEOUT_MS;
        active_readiness_lost(computer, "lost encryption");
    }
    progress_switch();
    log_readiness(computer);
}

static void hci_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type != HCI_EVENT_PACKET) return;
    computer_t *computer;
    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet) == GAP_SUBEVENT_LE_CONNECTION_COMPLETE)
                connected(packet);
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            computer = computer_for_handle(hci_event_disconnection_complete_get_connection_handle(packet));
            if (computer == NULL) break;
            printf("[output] PC%u handle=0x%04x disconnected reason=0x%02x address=%s "
                   "release-pending=0x%02x\n", slot_of(computer) + 1, computer->handle,
                   hci_event_disconnection_complete_get_reason(packet),
                   bd_addr_to_str(computer->address), computer->release_pending);
            computer->occupied = false;
            clear_queue(computer);
            computer->send_registered = false;
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
    printf("[output] PC%u handle=0x%04x %s status=0x%02x\n",
           slot_of(computer) + 1, handle, operation, status);
    refresh_security(computer);
}

static void service(btstack_timer_source_t *timer) {
    uint32_t now = btstack_run_loop_get_time_ms();
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) {
        computer_t *computer = &computers[i];
        if (!computer->occupied) continue;
        refresh_security(computer);
        if (!computer->disconnecting && computer->key_size < 16
            && (int32_t)(now - computer->auth_deadline) >= 0) {
            printf("[output] PC%u handle=0x%04x SECURITY TIMEOUT: disconnecting "
                   "unauthenticated/short-key link, not an inactive ready PC\n",
                   i + 1, computer->handle);
            require_release(computer);
            uint8_t status = gap_disconnect(computer->handle);
            computer->disconnecting = status == ERROR_CODE_SUCCESS;
            if (status != 0) {
                printf("[output] security-timeout disconnect failed=0x%02x\n", status);
                computer->auth_deadline = now + 1000;
            }
        }
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

void hub_output_status(void) {
    printf("[output] selected=PC%u switching=%u advertising-requested=%u last-adv-status=0x%02x\n",
           active_slot + 1, switch_pending, advertising_requested, advertising_status);
    for (unsigned i = 0; i < HUB_COMPUTER_COUNT; ++i) {
        computer_t *computer = &computers[i];
        if (!computer->occupied) {
            printf("[output] PC%u vacant%s\n", i + 1, active_slot == i ? " SELECTED" : "");
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
    }
}
