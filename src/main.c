#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack.h"
#include "ble/le_device_db.h"
#include "pico/bootrom.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "hub_config.h"
#include "hub_button.h"
#include "hub_input.h"
#include "hub_output.h"
#include "hub_passkey.h"
#include "hub_router.h"
#include "hub_security.h"
#include "hub_smp_decode.h"
#include "hub_security_policy.h"

typedef enum {
    LINK_CLASSIC,
    LINK_LE_CENTRAL,
    LINK_LE_PERIPHERAL
} link_role_t;

typedef struct {
    bool used;
    bool pairing;
    uint8_t classic_peer_io;
    uint16_t handle;
    link_role_t role;
    bd_addr_t address;
} link_t;

typedef enum {
    AUTH_NONE,
    AUTH_LE_CONFIRM,
    AUTH_LE_PASSKEY,
    AUTH_CLASSIC_CONFIRM,
    AUTH_CLASSIC_PASSKEY,
    AUTH_CLASSIC_PIN
} auth_kind_t;

typedef struct {
    auth_kind_t kind;
    uint32_t ticket;
    uint32_t deadline;
    uint16_t handle;
    bd_addr_t address;
} auth_request_t;

static link_t links[MAX_NR_HCI_CONNECTIONS];
static auth_request_t auth_requests[MAX_NR_HCI_CONNECTIONS];
static btstack_packet_callback_registration_t hci_registration;
static btstack_packet_callback_registration_t sm_registration;
static btstack_timer_source_t console_timer;
static bool working;
static bool pairing_enabled;
static uint32_t pairing_deadline;
static uint32_t next_ticket;
static bool boot_pending;
static uint32_t boot_deadline;
static bool keyboard_pairing_mode;
static bool just_works_pairing_mode;
static bool computer_pairing_policy;
static hub_passkey_t keyboard_passkey;
static uint16_t keyboard_auth_handle = HCI_CON_HANDLE_INVALID;
static uint32_t keyboard_auth_ticket;
static uint32_t keyboard_auth_deadline;
static hub_button_t enrollment_button;
static uint32_t next_button_sample;
static bool button_reset_accepted;
static int last_status_led = -1;

static bool ble_pairing_active(void) {
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        if (links[i].used && links[i].pairing) return true;
    }
    return false;
}

static void set_pairing_active(uint16_t handle, bool active) {
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        if (links[i].used && links[i].handle == handle) links[i].pairing = active;
    }
}

static void refresh_pairing_mode(void) {
    if (computer_pairing_policy && !pairing_enabled
        && !keyboard_passkey.active && !ble_pairing_active()) {
        bool was_keyboard = keyboard_pairing_mode;
        keyboard_pairing_mode = false;
        just_works_pairing_mode = false;
        computer_pairing_policy = false;
        hub_security_configure(HUB_SECURITY_INPUTS);
        if (was_keyboard) {
            hub_router_release_all();
            printf("[security] keyboard pairing ended; release all keys/buttons to resume\n");
        }
    }
}

static void finish_keyboard_capture(uint16_t handle) {
    if (!keyboard_passkey.active || handle != keyboard_auth_handle) return;
    hub_passkey_end(&keyboard_passkey);
    keyboard_auth_handle = HCI_CON_HANDLE_INVALID;
    keyboard_auth_ticket = 0;
    hub_router_release_all();
}

static void abort_keyboard_capture(const char *reason) {
    if (!keyboard_passkey.active) return;
    uint16_t handle = keyboard_auth_handle;
    printf("[security] keyboard passkey cancelled: %s\n", reason);
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        if (auth_requests[i].ticket == keyboard_auth_ticket) auth_requests[i].kind = AUTH_NONE;
    }
    finish_keyboard_capture(handle);
    sm_bonding_decline(handle);
}

bool hub_security_pairing_open(void) {
    return pairing_enabled
        && (int32_t)(btstack_run_loop_get_time_ms() - pairing_deadline) < 0;
}

bool hub_security_allow_pairing(uint16_t handle) {
    if (hub_input_is_connection(handle)) {
        return !computer_pairing_policy && hub_input_allow_pairing(handle);
    }
    return hub_security_pairing_open();
}

bool hub_security_input_pairing_blocked(void) {
    return computer_pairing_policy;
}

bool hub_security_capture_active(void) {
    /* Block the entire enrollment window, including before a passkey request,
       so early digits cannot leak into a previously selected computer. */
    return keyboard_pairing_mode;
}

bool hub_security_keyboard(unsigned input, const uint8_t report[8]) {
    bool consumed = hub_security_capture_active();
    if (keyboard_passkey.active && (!hub_security_pairing_open()
        || (int32_t)(btstack_run_loop_get_time_ms() - keyboard_auth_deadline) >= 0)) {
        abort_keyboard_capture("authorization expired");
    }
    uint32_t value = 0;
    hub_passkey_event_t event = hub_passkey_feed(&keyboard_passkey, input, report, &value);
    switch (event) {
        case HUB_PASSKEY_EDITED:
            printf("[security] passkey digits=%u/6 (not echoed)\n", keyboard_passkey.count);
            break;
        case HUB_PASSKEY_SUBMIT:
            for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
                if (auth_requests[i].ticket == keyboard_auth_ticket) {
                    auth_requests[i].kind = AUTH_NONE;
                }
            }
            printf("[security] keyboard passkey submitted; waiting for pairing result\n");
            sm_passkey_input(keyboard_auth_handle, value);
            break;
        case HUB_PASSKEY_CANCEL:
            pairing_enabled = false;
            abort_keyboard_capture("Escape pressed");
            break;
        case HUB_PASSKEY_INCOMPLETE:
            printf("[security] enter exactly six digits before Enter\n");
            break;
        case HUB_PASSKEY_INVALID:
            printf("[security] ambiguous/non-digit input; release keys and retype six digits\n");
            break;
        default:
            break;
    }
    return consumed;
}

void hub_security_input_disconnected(unsigned input) {
    if (hub_passkey_disconnected(&keyboard_passkey, input)) {
        pairing_enabled = false;
        abort_keyboard_capture("the typing keyboard disconnected");
    }
}

static void print_link_counts(void) {
    unsigned counts[3] = {0};
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        if (links[i].used) ++counts[links[i].role];
    }
    printf("[links] total=%u classic=%u le-central=%u le-peripheral=%u (goal: 6)\n",
           counts[0] + counts[1] + counts[2], counts[0], counts[1], counts[2]);
}

static void track_link(uint16_t handle, link_role_t role, const bd_addr_t address) {
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        if (!links[i].used) {
            links[i].used = true;
            links[i].pairing = false;
            links[i].classic_peer_io = SSP_IO_CAPABILITY_UNKNOWN;
            links[i].handle = handle;
            links[i].role = role;
            memcpy(links[i].address, address, sizeof(bd_addr_t));
            printf("[link] handle=0x%04x role=%s address=%s\n", handle,
                   role == LINK_CLASSIC ? "classic"
                   : role == LINK_LE_CENTRAL ? "LE-central" : "LE-peripheral",
                   bd_addr_to_str(address));
            print_link_counts();
            return;
        }
    }
    printf("[link] diagnostic table full for handle=0x%04x\n", handle);
}

static bool auth_allowed(const auth_request_t *request) {
    if (request->kind == AUTH_LE_CONFIRM || request->kind == AUTH_LE_PASSKEY) {
        return hub_security_allow_pairing(request->handle);
    }
    return hub_input_is_expected_classic(request->address);
}

static void decline_auth(auth_request_t *request) {
    hub_pairing_note("reply-declined", request->ticket, (int)request->kind);
    if (request->ticket == keyboard_auth_ticket) finish_keyboard_capture(request->handle);
    switch (request->kind) {
        case AUTH_LE_CONFIRM:
        case AUTH_LE_PASSKEY:
            sm_bonding_decline(request->handle);
            break;
        case AUTH_CLASSIC_CONFIRM:
            gap_ssp_confirmation_negative(request->address);
            break;
        case AUTH_CLASSIC_PASSKEY:
            gap_ssp_passkey_negative(request->address);
            break;
        case AUTH_CLASSIC_PIN:
            gap_pin_code_negative(request->address);
            break;
        default:
            break;
    }
    request->kind = AUTH_NONE;
}

static void close_pairing_window(const char *reason) {
    pairing_enabled = false;
    abort_keyboard_capture(reason);
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        auth_request_t *request = &auth_requests[i];
        if ((request->kind == AUTH_LE_CONFIRM || request->kind == AUTH_LE_PASSKEY)
            && !hub_input_is_connection(request->handle)) {
            decline_auth(request);
        }
    }
    refresh_pairing_mode();
    printf("[security] PC pairing closed: %s\n", reason);
}

static void start_input_enrollment(void) {
    if (hub_security_pairing_open() || computer_pairing_policy) {
        printf("[input] close PC pairing with pair off before input enrollment\n");
        return;
    }
    if (!hub_input_enroll_start()) printf("[input] enrollment request was not accepted\n");
}

static bool forget_inputs(unsigned mask) {
    close_pairing_window("input reset requested");
    hub_router_release_all();
    bool accepted = hub_input_forget(mask);
    printf("[input] forget mask=0x%x accepted=%u; computer bonds are not reset\n",
           mask, accepted);
    return accepted;
}

static void poll_enrollment_button(uint32_t now) {
    if (!working || boot_pending || (int32_t)(now - next_button_sample) < 0) return;
    next_button_sample = now + 25u;
    hub_button_action_t action = hub_button_tick(&enrollment_button, hub_bootsel_pressed(), now);
    if (action == HUB_BUTTON_ENROLL) {
        printf("[button] short press: requesting input enrollment\n");
        start_input_enrollment();
    } else if (action == HUB_BUTTON_RESET_INPUTS) {
        printf("[button] eight-second hold: forgetting both inputs, not computers\n");
        button_reset_accepted = forget_inputs((1u << HUB_INPUT_COUNT) - 1u);
    }
    bool reset_feedback = enrollment_button.stable_pressed
        && enrollment_button.reset_sent && button_reset_accepted;
    bool led = reset_feedback ? (now / 75u) % 2u
                              : hub_input_enrolling() ? (now / 250u) % 2u : true;
    if (last_status_led != (int)led) {
        cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led);
        last_status_led = led;
    }
}

static uint8_t classic_peer_io(const bd_addr_t address) {
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        if (links[i].used && links[i].role == LINK_CLASSIC
            && memcmp(links[i].address, address, 6) == 0) return links[i].classic_peer_io;
    }
    return SSP_IO_CAPABILITY_UNKNOWN;
}

static void request_auth(auth_kind_t kind, uint16_t handle,
                         const bd_addr_t address, uint32_t number) {
    auth_request_t candidate = {
        .kind = kind,
        .handle = handle,
        .deadline = btstack_run_loop_get_time_ms() + 60000u,
    };
    if (address != NULL) memcpy(candidate.address, address, sizeof(bd_addr_t));
    if (!auth_allowed(&candidate)) {
        printf("[security] unsolicited pairing rejected\n");
        decline_auth(&candidate);
        return;
    }
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        if (auth_requests[i].kind == AUTH_NONE) {
            candidate.ticket = ++next_ticket;
            if (candidate.ticket == 0) candidate.ticket = ++next_ticket;
            auth_requests[i] = candidate;
            hub_pairing_note("reply-requested", candidate.ticket, (int)kind);
            if (keyboard_pairing_mode && kind == AUTH_LE_PASSKEY
                && !hub_input_is_connection(handle)) {
                if (!hub_input_keyboard_ready() || !hub_passkey_begin(&keyboard_passkey)) {
                    printf("[security] no ready keyboard or another passkey is pending\n");
                    decline_auth(&auth_requests[i]);
                    return;
                }
                keyboard_auth_handle = handle;
                keyboard_auth_ticket = candidate.ticket;
                keyboard_auth_deadline = candidate.deadline;
                hub_router_release_all();
                printf("[security] PC handle=0x%04x: release keys, type its six-digit code "
                       "on the connected keyboard, then Enter. Backspace edits; Escape cancels.\n",
                       handle);
            }
            if (kind == AUTH_LE_CONFIRM || kind == AUTH_CLASSIC_CONFIRM) {
                printf("[security] compare %06" PRIu32
                       "; if it matches, enter: confirm %" PRIu32
                       " yes (or no)\n", number, candidate.ticket);
            } else if (kind == AUTH_CLASSIC_PIN) {
                printf("[security] legacy PIN for %s; enter: pin %" PRIu32
                       " <PIN, 1..16 characters>\n",
                       bd_addr_to_str(candidate.address), candidate.ticket);
            } else {
                printf("[security] enter the remote passkey: passkey %" PRIu32
                       " <six decimal digits>\n", candidate.ticket);
            }
            return;
        }
    }
    printf("[security] pending-request table full; pairing declined\n");
    decline_auth(&candidate);
}

static void clear_auth_for_link(uint16_t handle) {
    if (handle == keyboard_auth_handle) {
        pairing_enabled = false;
        finish_keyboard_capture(handle);
    }
    const uint8_t *address = NULL;
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        if (links[i].used && links[i].handle == handle) {
            address = links[i].address;
            links[i].used = false;
            break;
        }
    }
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        auth_request_t *request = &auth_requests[i];
        bool le = request->kind == AUTH_LE_CONFIRM || request->kind == AUTH_LE_PASSKEY;
        if ((le && request->handle == handle)
            || (!le && address != NULL && memcmp(request->address, address, 6) == 0)) {
            request->kind = AUTH_NONE;
        }
    }
}

static void clear_completed_auth(uint16_t handle, const uint8_t *address) {
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        auth_request_t *request = &auth_requests[i];
        bool le = request->kind == AUTH_LE_CONFIRM || request->kind == AUTH_LE_PASSKEY;
        if ((le && handle != HCI_CON_HANDLE_INVALID && request->handle == handle)
            || (!le && address != NULL && memcmp(request->address, address, 6) == 0)) {
            request->kind = AUTH_NONE;
        }
    }
}

static void security_event(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size) {
    (void)channel;
    (void)size;
    if (packet_type != HCI_EVENT_PACKET) return;
    uint16_t handle;
    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_PAIRING_STARTED:
            set_pairing_active(sm_event_pairing_started_get_handle(packet), true);
            break;
        case SM_EVENT_JUST_WORKS_REQUEST:
            handle = sm_event_just_works_request_get_handle(packet);
            if (keyboard_pairing_mode && !hub_input_is_connection(handle)) {
                printf("[security] keyboard mode requires passkey authentication; rejecting Just Works\n");
                sm_bonding_decline(handle);
            } else if (hub_security_allow_pairing(handle)) {
                printf("[security] Just Works accepted for 0x%04x (no MITM protection)\n",
                       handle);
                sm_just_works_confirm(handle);
            } else {
                printf("[security] pairing window closed; declining 0x%04x\n", handle);
                sm_bonding_decline(handle);
            }
            break;
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            request_auth(AUTH_LE_CONFIRM,
                         sm_event_numeric_comparison_request_get_handle(packet), NULL,
                         sm_event_numeric_comparison_request_get_passkey(packet));
            break;
        case SM_EVENT_PASSKEY_INPUT_NUMBER:
            request_auth(AUTH_LE_PASSKEY,
                         sm_event_passkey_input_number_get_handle(packet), NULL, 0);
            break;
        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
            handle = sm_event_passkey_display_number_get_handle(packet);
            if (hub_security_allow_pairing(handle)) {
                printf("[security] type %06" PRIu32 " on the remote device for 0x%04x\n",
                       sm_event_passkey_display_number_get_passkey(packet), handle);
            } else {
                sm_bonding_decline(handle);
            }
            break;
        case SM_EVENT_PAIRING_COMPLETE:
            handle = sm_event_pairing_complete_get_handle(packet);
            set_pairing_active(handle, false);
            if ((keyboard_pairing_mode || just_works_pairing_mode)
                && !hub_input_is_connection(handle)) {
                pairing_enabled = false;
            }
            finish_keyboard_capture(handle);
            printf("[security] pairing handle=0x%04x status=0x%02x reason=0x%02x\n",
                   sm_event_pairing_complete_get_handle(packet),
                   sm_event_pairing_complete_get_status(packet),
                   sm_event_pairing_complete_get_reason(packet));
            clear_completed_auth(sm_event_pairing_complete_get_handle(packet), NULL);
            refresh_pairing_mode();
            break;
        case SM_EVENT_REENCRYPTION_COMPLETE:
            printf("[security] re-encryption handle=0x%04x status=0x%02x\n",
                   sm_event_reencryption_complete_get_handle(packet),
                   sm_event_reencryption_complete_get_status(packet));
            break;
        default:
            break;
    }
}

static void controller_event(uint8_t packet_type, uint16_t channel,
                             uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type != HCI_EVENT_PACKET) return;
    bd_addr_t address;
    uint8_t status;
    uint16_t handle;
    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING && !working) {
                working = true;
                hub_output_start();
                hub_input_start();
                cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
                printf("\nPico 2 W Bluetooth Hub %s ready. Enter help.\n", PICO_PROGRAM_VERSION_STRING);
                if (HUB_CLASSIC_OPEN_TEST) {
                    printf("[open-test] Classic Level 0, mode 2; only saved explicit bridge authorizations restore\n");
                }
                printf("Pairing is closed. No input is generated automatically.\n");
                printf("BOOTSEL: short press enrolls inputs; eight-second hold forgets inputs only.\n");
            }
            break;
        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet)
                != GAP_SUBEVENT_LE_CONNECTION_COMPLETE) break;
            status = gap_subevent_le_connection_complete_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                printf("[HCI] LE connection failed status=0x%02x\n", status);
                break;
            }
            gap_subevent_le_connection_complete_get_peer_address(packet, address);
            track_link(gap_subevent_le_connection_complete_get_connection_handle(packet),
                       gap_subevent_le_connection_complete_get_role(packet) == 0
                           ? LINK_LE_CENTRAL : LINK_LE_PERIPHERAL,
                       address);
            break;
        case HCI_EVENT_CONNECTION_COMPLETE:
            status = hci_event_connection_complete_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                printf("[HCI] Classic connection failed status=0x%02x\n", status);
                break;
            }
            hci_event_connection_complete_get_bd_addr(packet, address);
            track_link(hci_event_connection_complete_get_connection_handle(packet),
                       LINK_CLASSIC, address);
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            handle = hci_event_disconnection_complete_get_connection_handle(packet);
            printf("[HCI] disconnected handle=0x%04x reason=0x%02x\n", handle,
                   hci_event_disconnection_complete_get_reason(packet));
            clear_auth_for_link(handle);
            print_link_counts();
            break;
        case HCI_EVENT_COMMAND_STATUS:
            status = hci_event_command_status_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                printf("[HCI] command=0x%04x status=0x%02x\n",
                       hci_event_command_status_get_command_opcode(packet), status);
            }
            break;
        case HCI_EVENT_COMMAND_COMPLETE:
            if (size >= 6 && packet[5] != ERROR_CODE_SUCCESS) {
                printf("[HCI] command=0x%04x completion=0x%02x\n",
                       hci_event_command_complete_get_command_opcode(packet), packet[5]);
            }
            break;
        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, address);
            if (hub_input_open_test_peer(address)) {
                printf("[open-test] PIN requested despite local Level 0; no PIN supplied\n");
                int result = gap_pin_code_negative(address);
                hub_pairing_note("open-test-PIN-refused", 0, result);
                break;
            }
            request_auth(AUTH_CLASSIC_PIN, HCI_CON_HANDLE_INVALID, address, 0);
            break;
        case HCI_EVENT_IO_CAPABILITY_RESPONSE:
            hci_event_io_capability_response_get_bd_addr(packet, address);
            for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
                if (links[i].used && links[i].role == LINK_CLASSIC
                    && memcmp(links[i].address, address, 6) == 0) {
                    links[i].classic_peer_io =
                        hci_event_io_capability_response_get_io_capability(packet);
                }
            }
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, address);
            if (hub_input_open_test_peer(address) || (hub_input_is_expected_classic(address)
                && classic_peer_io(address) == SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT)) {
                printf("[security] selected Classic peer %s: Just Works accepted (no MITM)\n",
                       bd_addr_to_str(address));
                int result = gap_ssp_confirmation_response(address);
                if (result) printf("[security] Classic confirmation failed=0x%02x\n", result);
            } else {
                request_auth(AUTH_CLASSIC_CONFIRM, HCI_CON_HANDLE_INVALID, address,
                             hci_event_user_confirmation_request_get_numeric_value(packet));
            }
            break;
        case HCI_EVENT_USER_PASSKEY_REQUEST:
            hci_event_user_passkey_request_get_bd_addr(packet, address);
            request_auth(AUTH_CLASSIC_PASSKEY, HCI_CON_HANDLE_INVALID, address, 0);
            break;
        case HCI_EVENT_USER_PASSKEY_NOTIFICATION:
            hci_event_user_passkey_notification_get_bd_addr(packet, address);
            if (hub_input_is_expected_classic(address)) {
                printf("[security] type %06" PRIu32 " on %s\n",
                       hci_event_user_passkey_notification_get_numeric_value(packet),
                       bd_addr_to_str(address));
            } else {
                printf("[security] unexpected Classic passkey notification\n");
            }
            break;
        case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
            hci_event_simple_pairing_complete_get_bd_addr(packet, address);
            printf("[security] Classic pairing %s status=0x%02x\n",
                   bd_addr_to_str(address),
                   hci_event_simple_pairing_complete_get_status(packet));
            clear_completed_auth(HCI_CON_HANDLE_INVALID, address);
            break;
        case HCI_EVENT_HARDWARE_ERROR:
            printf("[HCI] CONTROLLER HARDWARE ERROR 0x%02x\n", size > 2 ? packet[2] : 0);
            break;
        default:
            break;
    }
}

static bool parse_uint(const char *text, unsigned long max, int base,
                       unsigned long *value) {
    if (text == NULL || *text == '\0' || *text == '-' || *text == '+') return false;
    errno = 0;
    char *end;
    unsigned long parsed = strtoul(text, &end, base);
    if (errno != 0 || *end != '\0' || parsed > max) return false;
    *value = parsed;
    return true;
}

static bool parse_signed(const char *text, long min, long max, long *value) {
    if (text == NULL || *text == '\0') return false;
    errno = 0;
    char *end;
    long parsed = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed < min || parsed > max) return false;
    *value = parsed;
    return true;
}

static void print_help(void) {
    printf(
        "\nCommands (computer/input slots are 1-based):\n"
        "  help | status | bonds | diag\n"
        "  diag                       Replay recent pairing metadata (no PIN/key values)\n"
        "  enroll | enroll stop       Timed automatic keyboard/mouse enrollment\n"
        "  inputs reset               Forget both inputs, then enroll replacements\n"
        "  input forget <1|2>         Forget only that input, preserve the other\n"
        "  pair keyboard              Pair a PC using an already-connected keyboard\n"
        "  pair justworks             Pair one PC without code entry (encrypted, no MITM)\n"
        "  pair on | pair off         Console pairing / close window (120 seconds)\n"
        "  confirm <ticket> yes|no     Verify numbers on both devices first\n"
        "  passkey <ticket> <123456>   Supply a six-digit remote passkey\n"
        "  pin <ticket> <PIN>          Legacy Classic PIN, 1..16 characters\n"
        "  scan ble | scan classic | scan stop\n"
        "  name <address>             Read a Classic device's Bluetooth name\n"
        "  open-test <1|2> <address>   Isolated Classic Level 0 test (test image only)\n"
        "  bridge on|off              Save Level 0 target + forwarding consent, or persistently disable it\n"
        "  connect <1|2> classic <AA:BB:CC:DD:EE:FF>\n"
        "  connect <1|2> ble public|random <AA:BB:CC:DD:EE:FF>\n"
        "  disconnect <1|2>\n"
        "  select <1..4>              Keep all PC connections established\n"
        "  tap <HID-code>             EXPLICIT test key, e.g. tap 0x04 = A\n"
        "  move <dx> <dy>             EXPLICIT test mouse motion, +/-1000\n"
        "  release                    Emergency all-keys/buttons release\n"
        "  unbond classic|public|random <address>  Requires no live links\n"
        "  bootsel                    Reboot to USB firmware drive\n"
        "Shortcut: Ctrl+Alt+Right/Left, then release all keys/buttons.\n"
        "BOOTSEL while running: short=enroll; hold 8 seconds=input-only reset.\n\n");
}

static void print_bonds(void) {
    bd_addr_t address;
    sm_key_t irk;
    printf("[bonds] BLE entries=%d (keys are not printed)\n", le_device_db_count());
    for (int i = 0; i < le_device_db_max_count(); ++i) {
        int type;
        le_device_db_info(i, &type, address, irk);
        if (type != BD_ADDR_TYPE_UNKNOWN) {
            printf("  BLE type=%s address=%s\n",
                   type == BD_ADDR_TYPE_LE_PUBLIC ? "public" : "random",
                   bd_addr_to_str(address));
        }
    }
    btstack_link_key_iterator_t iterator;
    if (!gap_link_key_iterator_init(&iterator)) {
        printf("[bonds] Classic key enumeration unavailable\n");
        return;
    }
    link_key_t key;
    link_key_type_t type;
    while (gap_link_key_iterator_get_next(&iterator, address, key, &type)) {
        printf("  Classic address=%s\n", bd_addr_to_str(address));
    }
    gap_link_key_iterator_done(&iterator);
}

static bool auth_command(const char *command, const char *ticket_text,
                         const char *answer) {
    unsigned long ticket;
    if (!parse_uint(ticket_text, UINT32_MAX, 10, &ticket)) return false;
    auth_request_t *request = NULL;
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        if (auth_requests[i].kind != AUTH_NONE && auth_requests[i].ticket == ticket) {
            request = &auth_requests[i];
            break;
        }
    }
    if (request == NULL) {
        printf("[security] ticket not found or expired\n");
        return true;
    }
    if (!auth_allowed(request)
        || (int32_t)(btstack_run_loop_get_time_ms() - request->deadline) >= 0) {
        printf("[security] authorization expired; request declined\n");
        decline_auth(request);
        return true;
    }
    int response_status = ERROR_CODE_SUCCESS;
    if (strcmp(command, "confirm") == 0
        && (request->kind == AUTH_LE_CONFIRM || request->kind == AUTH_CLASSIC_CONFIRM)) {
        if (strcmp(answer, "no") == 0) {
            decline_auth(request);
            return true;
        }
        if (strcmp(answer, "yes") != 0) return false;
        if (request->kind == AUTH_LE_CONFIRM) sm_numeric_comparison_confirm(request->handle);
        else response_status = gap_ssp_confirmation_response(request->address);
    } else if (strcmp(command, "passkey") == 0
               && (request->kind == AUTH_LE_PASSKEY
                   || request->kind == AUTH_CLASSIC_PASSKEY)) {
        unsigned long passkey;
        if (strlen(answer) != 6 || !parse_uint(answer, 999999, 10, &passkey)) return false;
        if (request->kind == AUTH_LE_PASSKEY) sm_passkey_input(request->handle, passkey);
        else response_status = gap_ssp_passkey_response(request->address, passkey);
        if (request->ticket == keyboard_auth_ticket) hub_passkey_submitted(&keyboard_passkey);
    } else if (strcmp(command, "pin") == 0 && request->kind == AUTH_CLASSIC_PIN) {
        if (strlen(answer) == 0 || strlen(answer) > 16) return false;
        hub_pairing_note("PIN-submit", request->ticket, (int)strlen(answer));
        response_status = gap_pin_code_response(request->address, answer);
    } else {
        return false;
    }
    hub_pairing_note("reply-api-result", request->ticket, response_status);
    if (response_status != ERROR_CODE_SUCCESS) {
        printf("[security] response NOT queued for ticket %" PRIu32
               ": status=0x%02x\n", request->ticket, response_status);
    } else {
        printf("[security] response queued for ticket %" PRIu32
               "; authentication is not complete yet\n", request->ticket);
    }
    request->kind = AUTH_NONE;
    return true;
}

static bool test_input_allowed(void) {
    if (hub_security_capture_active()) {
        printf("[console] test input rejected during keyboard pairing\n");
        return false;
    }
    if (hub_output_switching()) {
        printf("[console] test input rejected during switch\n");
        return false;
    }
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        if (links[i].used && links[i].role != LINK_LE_PERIPHERAL) {
            printf("[console] disconnect physical input peers before using tap/move\n");
            return false;
        }
    }
    return true;
}

static void console_command(char *line) {
    char *args[7];
    unsigned argc = 0;
    char *cursor = line;
    while (*cursor) {
        while (isspace((unsigned char)*cursor)) ++cursor;
        if (!*cursor) break;
        if (argc == sizeof(args) / sizeof(args[0])) {
            printf("[console] too many arguments\n");
            return;
        }
        args[argc++] = cursor;
        while (*cursor && !isspace((unsigned char)*cursor)) ++cursor;
        if (*cursor) *cursor++ = '\0';
    }
    if (argc == 0) return;
    if (boot_pending) {
        printf("[console] reboot already pending\n");
        return;
    }
    if (strcmp(args[0], "help") == 0 && argc == 1) {
        print_help();
        return;
    }
    if (strcmp(args[0], "bootsel") == 0 && argc == 1) {
        hub_router_stop();
        hub_input_enroll_stop();
        close_pairing_window("reboot");
        boot_pending = true;
        boot_deadline = btstack_run_loop_get_time_ms() + 300u;
        printf("[console] entering BOOTSEL; USB serial will disconnect\n");
        return;
    }
    if (!working) {
        printf("[console] Bluetooth is not ready\n");
        return;
    }
    unsigned long number;
    if (strcmp(args[0], "status") == 0 && argc == 1) {
        printf("[status] firmware=%s\n", PICO_PROGRAM_VERSION_STRING);
        printf("[status] uptime-ms=%" PRIu32 " pairing=%s mode=%s keyboard-ready=%u passkey-pending=%u\n",
               btstack_run_loop_get_time_ms(),
               hub_security_pairing_open() ? "open" : "closed",
               keyboard_pairing_mode ? "physical-keyboard"
                                     : just_works_pairing_mode ? "JustWorks" : "console",
               hub_input_keyboard_ready(), keyboard_passkey.active);
        printf("[status] input-enrollment=%u BOOTSEL=%u\n",
               hub_input_enrolling(), enrollment_button.stable_pressed);
        printf("[status] classic-level=%u classic-mode=%u open-test-build=%u\n",
               gap_get_security_level(), gap_get_security_mode(), HUB_CLASSIC_OPEN_TEST);
        printf("[status] open-input-bridge=%u\n", hub_input_open_bridge_enabled());
        print_link_counts();
        hub_output_status();
        hub_input_status();
        hub_router_status();
        return;
    }
    if (strcmp(args[0], "bonds") == 0 && argc == 1) {
        print_bonds();
        return;
    }
    if (strcmp(args[0], "diag") == 0 && argc == 1) {
        hub_pairing_diagnostics();
        return;
    }
    if (strcmp(args[0], "enroll") == 0) {
        if (argc == 1) {
            start_input_enrollment();
            return;
        }
        if (argc == 2 && strcmp(args[1], "stop") == 0) {
            hub_input_enroll_stop();
            return;
        }
    }
    if (argc == 2 && strcmp(args[0], "inputs") == 0 && strcmp(args[1], "reset") == 0) {
        forget_inputs((1u << HUB_INPUT_COUNT) - 1u);
        return;
    }
    if (argc == 3 && strcmp(args[0], "input") == 0 && strcmp(args[1], "forget") == 0
        && parse_uint(args[2], HUB_INPUT_COUNT, 10, &number) && number > 0) {
        forget_inputs(1u << (number - 1u));
        return;
    }
    if (strcmp(args[0], "bridge") == 0 && argc == 2) {
        if (strcmp(args[1], "off") == 0) {
            close_pairing_window("unauthenticated-input forwarding disabled");
            hub_input_set_open_bridge(false);
            return;
        }
        if (strcmp(args[1], "on") == 0) {
            hub_input_set_open_bridge(true);
            return;
        }
    }
    if (strcmp(args[0], "pair") == 0 && argc == 2) {
        if (HUB_CLASSIC_OPEN_TEST && !hub_input_open_bridge_enabled()
            && strcmp(args[1], "off") != 0 && strcmp(args[1], "justworks") != 0) {
            printf("[open-test] connect the Level 0 keyboard and explicitly enable bridge on first\n");
            return;
        }
        if (strcmp(args[1], "on") == 0 || strcmp(args[1], "keyboard") == 0
            || strcmp(args[1], "justworks") == 0) {
            bool use_keyboard = strcmp(args[1], "keyboard") == 0;
            bool use_just_works = strcmp(args[1], "justworks") == 0;
            if (ble_pairing_active() || keyboard_passkey.active || hub_input_pairing_busy()
                || hub_input_enrolling()) {
                printf("[security] finish input enrollment (enroll stop) and pending connections first\n");
                return;
            }
            if (use_keyboard && !hub_input_keyboard_ready()) {
                printf("[security] connect a keyboard and wait for HID-ready before pair keyboard\n");
                return;
            }
            keyboard_pairing_mode = use_keyboard;
            just_works_pairing_mode = use_just_works;
            computer_pairing_policy = true;
            hub_security_configure(use_just_works ? HUB_SECURITY_PC_JUST_WORKS
                : use_keyboard ? HUB_SECURITY_PC_KEYBOARD : HUB_SECURITY_PC_CONSOLE);
            pairing_deadline = btstack_run_loop_get_time_ms() + HUB_PAIRING_WINDOW_MS;
            pairing_enabled = true;
            if (use_keyboard) hub_router_release_all();
            printf("[security] PC pairing open for 120 seconds, mode=%s; close with pair off\n",
                   use_just_works ? "JustWorks (encrypted, no code entry, no MITM)"
                       : use_keyboard ? "physical keyboard (normal input paused)" : "console");
            return;
        }
        if (strcmp(args[1], "off") == 0) {
            close_pairing_window("operator");
            return;
        }
    }
    if (argc == 3 && (strcmp(args[0], "confirm") == 0
                      || strcmp(args[0], "passkey") == 0
                      || strcmp(args[0], "pin") == 0)) {
        if (auth_command(args[0], args[1], args[2])) return;
    }
    if (strcmp(args[0], "scan") == 0 && argc == 2) {
        if (strcmp(args[1], "stop") == 0) {
            hub_input_scan_stop();
            return;
        }
        if (strcmp(args[1], "classic") == 0 || strcmp(args[1], "ble") == 0) {
            hub_input_scan(strcmp(args[1], "classic") == 0);
            return;
        }
    }
    if (strcmp(args[0], "name") == 0 && argc == 2) {
        bd_addr_t address;
        if (strlen(args[1]) != 17 || !sscanf_bd_addr(args[1], address)) {
            printf("[name] invalid Bluetooth address\n");
            return;
        }
        if (hub_input_pairing_busy() || ble_pairing_active()) {
            printf("[name] finish active enrollment/connection first\n");
            return;
        }
        int result = gap_remote_name_request(address, 2, 0);
        printf("[name] request peer=%s status=0x%02x\n", bd_addr_to_str(address), result);
        return;
    }
    if (strcmp(args[0], "open-test") == 0 && argc == 3
        && parse_uint(args[1], HUB_INPUT_COUNT, 10, &number) && number > 0) {
        hub_input_open_test_connect((unsigned)number - 1, args[2]);
        return;
    }
    if (strcmp(args[0], "connect") == 0 && (argc == 4 || argc == 5)
        && parse_uint(args[1], HUB_INPUT_COUNT, 10, &number) && number > 0) {
        if (argc == 4 && strcmp(args[2], "classic") == 0) {
            hub_input_connect((unsigned)number - 1, true, 0, args[3]);
            return;
        }
        if (argc == 5 && strcmp(args[2], "ble") == 0
            && (strcmp(args[3], "public") == 0 || strcmp(args[3], "random") == 0)) {
            hub_input_connect((unsigned)number - 1, false,
                              strcmp(args[3], "random") == 0 ? 1 : 0, args[4]);
            return;
        }
    }
    if (strcmp(args[0], "disconnect") == 0 && argc == 2
        && parse_uint(args[1], HUB_INPUT_COUNT, 10, &number) && number > 0) {
        hub_input_disconnect((unsigned)number - 1);
        return;
    }
    if (strcmp(args[0], "select") == 0 && argc == 2
        && parse_uint(args[1], HUB_COMPUTER_COUNT, 10, &number) && number > 0) {
        hub_router_select((unsigned)number - 1);
        return;
    }
    if (strcmp(args[0], "release") == 0 && argc == 1) {
        hub_router_release_all();
        return;
    }
    if (strcmp(args[0], "tap") == 0 && argc == 2
        && parse_uint(args[1], 0xdf, 0, &number) && number >= 4) {
        if (!test_input_allowed()) return;
        uint8_t report[8] = {0, 0, (uint8_t)number};
        if (!hub_output_keyboard(report)) {
            printf("[console] test key rejected\n");
            return;
        }
        memset(report, 0, sizeof(report));
        if (!hub_output_keyboard(report)) hub_output_release_all();
        return;
    }
    if (strcmp(args[0], "move") == 0 && argc == 3) {
        long dx, dy;
        if (parse_signed(args[1], -1000, 1000, &dx)
            && parse_signed(args[2], -1000, 1000, &dy)) {
            if (!test_input_allowed()) return;
            if (!hub_output_mouse(0, (int32_t)dx, (int32_t)dy, 0)) {
                printf("[console] test motion rejected\n");
            }
            return;
        }
    }
    if (strcmp(args[0], "unbond") == 0 && argc == 3) {
        for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
            if (links[i].used) {
                printf("[security] disconnect inputs and PCs before removing a bond\n");
                return;
            }
        }
        bd_addr_t address;
        if (strlen(args[2]) != 17 || !sscanf_bd_addr(args[2], address)) {
            printf("[security] invalid Bluetooth address\n");
            return;
        }
        bd_addr_type_t type;
        if (strcmp(args[1], "classic") == 0) type = BD_ADDR_TYPE_ACL;
        else if (strcmp(args[1], "public") == 0) type = BD_ADDR_TYPE_LE_PUBLIC;
        else if (strcmp(args[1], "random") == 0) type = BD_ADDR_TYPE_LE_RANDOM;
        else goto invalid;
        gap_delete_bonding(type, address);
        printf("[security] removed local bond for %s; also remove it on the peer\n",
               bd_addr_to_str(address));
        return;
    }
invalid:
    printf("[console] invalid command/arguments; enter help\n");
}

static void console_poll(btstack_timer_source_t *timer) {
    static char line[192];
    static unsigned length;
    static bool discard;
    for (unsigned budget = 0; budget < 64; ++budget) {
        int ch = getchar_timeout_us(0);
        if (ch == PICO_ERROR_TIMEOUT) break;
        if (ch == '\r' || ch == '\n') {
            if (discard) printf("\n[console] line too long; discarded\n");
            else if (length) {
                line[length] = '\0';
                printf("\n");
                console_command(line);
            }
            length = 0;
            discard = false;
        } else if (ch == '\b' || ch == 127) {
            if (length && !discard) {
                --length;
                printf("\b \b");
            }
        } else if (ch >= 32 && ch <= 126 && !discard) {
            if (length + 1 >= sizeof(line)) discard = true;
            else {
                line[length++] = (char)ch;
                putchar(ch);
            }
        }
    }
    uint32_t now = btstack_run_loop_get_time_ms();
    if (boot_pending && (int32_t)(now - boot_deadline) >= 0) reset_usb_boot(0, 0);
    if (pairing_enabled && !hub_security_pairing_open()) {
        close_pairing_window("window expired");
    }
    if (keyboard_pairing_mode && pairing_enabled && !hub_input_keyboard_ready()) {
        close_pairing_window("no ready keyboard");
    }
    if (keyboard_passkey.active && (int32_t)(now - keyboard_auth_deadline) >= 0) {
        close_pairing_window("passkey timeout");
    }
    for (unsigned i = 0; i < MAX_NR_HCI_CONNECTIONS; ++i) {
        auth_request_t *request = &auth_requests[i];
        if (request->kind != AUTH_NONE
            && ((int32_t)(now - request->deadline) >= 0 || !auth_allowed(request))) {
            printf("[security] ticket %" PRIu32 " expired or authorization revoked; declined\n",
                   request->ticket);
            hub_pairing_note("reply-timeout-or-revoked", request->ticket, 0);
            decline_auth(request);
        }
    }
    refresh_pairing_mode();
    hub_router_poll();
    poll_enrollment_button(now);
    btstack_run_loop_set_timer(timer, 10);
    btstack_run_loop_add_timer(timer);
}

int main(void) {
    stdio_init_all();
    if (cyw43_arch_init() != PICO_OK) panic("CYW43439 initialization failed");
    hub_smp_trace_init();

    l2cap_init();
    sm_init();
    sdp_init();
    gatt_client_init();
    hub_security_configure(HUB_SECURITY_INPUTS);
    gap_set_local_name(HUB_DEVICE_NAME);
    gap_set_class_of_device(0x000100);
    gap_discoverable_control(0);
    gap_connectable_control(1);
    gap_set_bondable_mode(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
    gap_ssp_set_authentication_requirement(
        SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_ssp_set_auto_accept(0);
    if (HUB_CLASSIC_OPEN_TEST) {
        if (gap_set_security_mode(GAP_SECURITY_MODE_2) != ERROR_CODE_SUCCESS) {
            panic("Cannot enable experimental Classic security mode");
        }
        gap_set_security_level(LEVEL_0);
        gap_set_secure_connections_only_mode(false);
        gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
        gap_ssp_set_authentication_requirement(
            SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_NO_BONDING);
    }

    hub_passkey_init(&keyboard_passkey);
    hub_button_init(&enrollment_button);
    hub_router_init();
    hci_registration.callback = controller_event;
    hci_add_event_handler(&hci_registration);
    sm_registration.callback = security_event;
    sm_add_event_handler(&sm_registration);
    hub_output_init();
    hub_input_init();
    btstack_run_loop_set_timer_handler(&console_timer, console_poll);
    btstack_run_loop_set_timer(&console_timer, 10);
    btstack_run_loop_add_timer(&console_timer);
    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();
    return 0;
}
