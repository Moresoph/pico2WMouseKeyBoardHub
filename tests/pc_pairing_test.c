#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "hub_config.h"
#include "hub_security_policy.h"
#include "hub_ui.h"
#include "ble/sm.h"

static struct { bool used, pairing; uint16_t handle; } links[MAX_NR_HCI_CONNECTIONS];
static struct { bool active; } keyboard_passkey;
static bool keyboard_pairing_mode, just_works_pairing_mode, computer_pairing_policy;
static bool pairing_enabled, working, boot_pending;
static uint32_t pairing_deadline, next_button_sample, clock_ms;
static bool input_busy, enrolling, keyboard_ready, capacity_ready, button_pressed;
static unsigned releases, prepares, confirmations, declines;
static bool sc_only;
static io_capability_t io;
static uint8_t auth, key_min, key_max;
static hub_security_profile_t configured;
#if HUB_DUAL_BLUETOOTH
static hub_ui_button_t runtime_button;
static uint32_t ui_ack_until, ui_error_until;
static hub_ui_fault_clock_t fault_clock;
static int last_status_led = -1;
enum { CYW43_WL_GPIO_LED_PIN = 0 };
#endif

void sm_set_secure_connections_only_mode(bool value) { sc_only = value; }
void sm_set_io_capabilities(io_capability_t value) { io = value; }
void sm_set_authentication_requirements(uint8_t value) { auth = value; }
void sm_set_encryption_key_size_range(uint8_t low, uint8_t high) {
    key_min = low; key_max = high;
}

#define hub_security_configure configure_profile
#include "../src/hub_security_policy.c"
#undef hub_security_configure
bool hub_security_configure(hub_security_profile_t profile) {
    configured = profile;
    return configure_profile(profile);
}

uint32_t btstack_run_loop_get_time_ms(void) { return clock_ms; }
static bool hub_input_pairing_busy(void) { return input_busy; }
static bool hub_input_enrolling(void) { return enrolling; }
static bool hub_input_keyboard_ready(void) { return keyboard_ready; }
static bool hub_input_is_connection(uint16_t handle) { return handle == 1; }
static bool hub_input_allow_pairing(uint16_t handle) { return handle == 1 && input_busy; }
static bool hub_input_open_bridge_enabled(void) { return false; }
static void hub_router_release_all(void) { ++releases; }
void sm_just_works_confirm(hci_con_handle_t handle) { assert(handle == 2); ++confirmations; }
void sm_bonding_decline(hci_con_handle_t handle) { (void)handle; ++declines; }
static uint16_t sm_event_just_works_request_get_handle(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}
static uint16_t sm_event_pairing_complete_get_handle(const uint8_t *p) {
    return sm_event_just_works_request_get_handle(p);
}
static uint8_t sm_event_pairing_complete_get_status(const uint8_t *p) { return p[2]; }
static uint8_t sm_event_pairing_complete_get_reason(const uint8_t *p) { return p[2]; }
static void finish_keyboard_capture(uint16_t handle) { (void)handle; keyboard_passkey.active = false; }
static void clear_completed_auth(uint16_t handle, const void *address) { (void)handle; (void)address; }

static bool hub_security_pairing_open(void);
static void close_pairing_window(const char *reason);
#if HUB_DUAL_BLUETOOTH
static bool hub_output_pairing_prepare(void) { ++prepares; return capacity_ready; }
static bool hub_bootsel_pressed(void) { return button_pressed; }
static unsigned hub_output_active_slot(void) { return 0; }
static bool hub_output_ready(unsigned slot) { (void)slot; return true; }
static void hub_router_select(unsigned slot) { (void)slot; }
static bool hub_input_link_online(void) { return true; }
static bool hub_output_storage_healthy(void) { return true; }
static bool hub_output_slot_saved(unsigned slot) { (void)slot; return true; }
static void cyw43_arch_gpio_put(unsigned pin, bool value) { (void)pin; (void)value; }
#endif

#include "pc_pairing_generated.inc"

static void close_pairing_window(const char *reason) {
    (void)reason;
    pairing_enabled = false;
    refresh_pairing_mode();
}

static void reset(void) {
    memset(links, 0, sizeof(links));
    keyboard_passkey.active = false;
    keyboard_pairing_mode = just_works_pairing_mode = computer_pairing_policy = false;
    pairing_enabled = boot_pending = input_busy = enrolling = keyboard_ready = false;
    capacity_ready = working = true;
    releases = prepares = confirmations = declines = 0;
    pairing_deadline = next_button_sample = 0;
    clock_ms = 100;
    configured = HUB_SECURITY_INPUTS;
    assert(!hub_security_pairing_open());
    assert(!hub_security_allow_pairing(2));
#if HUB_DUAL_BLUETOOTH
    memset(&runtime_button, 0, sizeof(runtime_button));
    memset(&fault_clock, 0, sizeof(fault_clock));
    ui_ack_until = ui_error_until = 0;
    button_pressed = false;
    last_status_led = -1;
#endif
}

static void assert_just_works(void) {
    assert(hub_security_pairing_open() && hub_security_allow_pairing(2));
    assert(just_works_pairing_mode && !keyboard_pairing_mode && computer_pairing_policy);
    assert(configured == HUB_SECURITY_PC_JUST_WORKS);
    assert(sc_only && io == IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    assert(auth == (SM_AUTHREQ_BONDING | SM_AUTHREQ_SECURE_CONNECTION));
    assert(key_min == 16 && key_max == 16);
    assert(pairing_deadline == clock_ms + HUB_PAIRING_WINDOW_MS);
    assert(!hub_security_allow_pairing(1) && !releases && !confirmations);
}

static void commands(void) {
    reset();
    pair_command("on");
#if HUB_DUAL_BLUETOOTH
    assert_just_works();
    assert(prepares == 1);
#else
    assert(configured == HUB_SECURITY_PC_CONSOLE);
    assert(io == IO_CAPABILITY_DISPLAY_YES_NO && !just_works_pairing_mode);
#endif
    pair_command("off");
    assert(!hub_security_pairing_open() && !computer_pairing_policy);
    pair_command("justworks");
    assert_just_works();
    clock_ms = pairing_deadline - 1;
    assert(hub_security_pairing_open());
    clock_ms++;
    assert(!hub_security_pairing_open() && !hub_security_allow_pairing(2));
    pair_command("off");
    assert(configured == HUB_SECURITY_INPUTS && !just_works_pairing_mode);
#if HUB_DUAL_BLUETOOTH
    pair_command("console");
    assert(configured == HUB_SECURITY_PC_CONSOLE && !just_works_pairing_mode);
    assert(io == IO_CAPABILITY_DISPLAY_YES_NO);
    pair_command("off");
#endif
    pair_command("keyboard");
    assert(!hub_security_pairing_open());
    keyboard_ready = true;
    pair_command("keyboard");
    assert(keyboard_pairing_mode && !just_works_pairing_mode && releases == 1);
    assert(configured == HUB_SECURITY_PC_KEYBOARD && sc_only);
    assert(io == IO_CAPABILITY_KEYBOARD_ONLY && (auth & SM_AUTHREQ_MITM_PROTECTION));
    pair_command("off");
    assert(releases == 2 && !computer_pairing_policy);
}

static void busy_gates_and_rollover(void) {
    for (unsigned state = 0; state < 4; ++state) {
        reset();
        links[0].used = links[0].pairing = state == 0;
        keyboard_passkey.active = state == 1;
        input_busy = state == 2;
        enrolling = state == 3;
        pair_command("on");
        assert(!pairing_enabled && !prepares && configured == HUB_SECURITY_INPUTS);
    }
#if HUB_DUAL_BLUETOOTH
    reset();
    capacity_ready = false;
    pair_command("on");
    assert(!pairing_enabled && prepares == 1 && configured == HUB_SECURITY_INPUTS);
#endif
    reset();
    clock_ms = UINT32_MAX - 500;
    pair_command("justworks");
    assert_just_works();
    clock_ms = pairing_deadline;
    assert(!hub_security_pairing_open());
}

static void association_and_completion(void) {
    reset();
    pairing_event(SM_EVENT_JUST_WORKS_REQUEST, 2, 0);
    assert(declines == 1 && !confirmations);
    pair_command("justworks");
    pairing_event(SM_EVENT_JUST_WORKS_REQUEST, 2, 0);
    assert(confirmations == 1 && declines == 1);
    pairing_event(SM_EVENT_PAIRING_COMPLETE, 1, 0);
    assert(hub_security_pairing_open());
    for (unsigned failed = 0; failed < 2; ++failed) {
        reset();
        pair_command("justworks");
        links[0].used = links[0].pairing = true;
        links[0].handle = 2;
        pairing_event(SM_EVENT_PAIRING_COMPLETE, 2, (uint8_t)failed);
        assert(!hub_security_pairing_open() && !links[0].pairing);
        assert(!computer_pairing_policy && configured == HUB_SECURITY_INPUTS);
        pairing_event(SM_EVENT_JUST_WORKS_REQUEST, 2, 0);
        assert(declines == 1 && !confirmations);
    }
    reset();
    pair_command("justworks");
    clock_ms = pairing_deadline;
    pairing_event(SM_EVENT_JUST_WORKS_REQUEST, 2, 0);
    assert(!confirmations && declines == 1);
    reset();
    keyboard_ready = true;
    pair_command("keyboard");
    pairing_event(SM_EVENT_JUST_WORKS_REQUEST, 2, 0);
    assert(!confirmations && declines == 1 && keyboard_pairing_mode);
}

#if HUB_DUAL_BLUETOOTH
static void button_tick(bool pressed, uint32_t now) {
    clock_ms = now;
    button_pressed = pressed;
    poll_enrollment_button(now);
}

static void physical_pairing(void) {
    reset();
    button_tick(false, 100);
    button_tick(true, 200);
    button_tick(true, 250);
    button_tick(true, 3250);
    assert(!pairing_enabled);
    button_tick(false, 3300);
    button_tick(false, 3350);
    assert_just_works();
    assert(!keyboard_ready && prepares == 1);
    button_tick(true, 3400);
    button_tick(true, 3450);
    button_tick(false, 3500);
    button_tick(false, 3550);
    button_tick(false, 3900);
    assert(!hub_security_pairing_open() && !computer_pairing_policy);
    reset();
    button_tick(true, 100);
    button_tick(true, 3200);
    button_tick(false, 3300);
    button_tick(false, 3350);
    assert(!pairing_enabled && !prepares);
}
#endif

int main(void) {
    commands();
    busy_gates_and_rollover();
    association_and_completion();
#if HUB_DUAL_BLUETOOTH
    physical_pairing();
#endif
    puts("PC pairing defaults, physical consent, expiry, encryption and passkey tests passed.");
    return 0;
}
