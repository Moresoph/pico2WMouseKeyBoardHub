#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "hub_config.h"
#include "hub_output.h"
#include "hub_router.h"
#include "hub_security.h"

static bool ready[HUB_COMPUTER_COUNT];
static bool switching;
static bool fail_output;
static unsigned active;
static unsigned pending;
static unsigned keyboard_count;
static unsigned mouse_count;
static unsigned release_count;
static unsigned select_count;
static uint8_t last_keyboard[8];
static uint8_t last_buttons;
static int32_t last_dx;
static bool capturing;
static bool finish_capture_on_report;
static unsigned captured_reports;

bool hub_security_capture_active(void) { return capturing; }
bool hub_security_keyboard(unsigned input, const uint8_t report[8]) {
    (void)input;
    (void)report;
    bool consumed = capturing;
    if (capturing) ++captured_reports;
    if (finish_capture_on_report) capturing = false;
    return consumed;
}
void hub_security_input_disconnected(unsigned input) { (void)input; }

bool hub_output_ready(unsigned slot) {
    return slot < HUB_COMPUTER_COUNT && ready[slot];
}

unsigned hub_output_active_slot(void) { return active; }
bool hub_output_switching(void) { return switching; }

bool hub_output_select(unsigned slot) {
    if (!hub_output_ready(slot) || switching) return false;
    pending = slot;
    switching = true;
    ++select_count;
    return true;
}

bool hub_output_keyboard(const uint8_t report[8]) {
    if (fail_output || switching || !ready[active]) return false;
    memcpy(last_keyboard, report, 8);
    ++keyboard_count;
    return true;
}

bool hub_output_mouse(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel) {
    (void)dy;
    (void)wheel;
    if (fail_output || switching || !ready[active]) return false;
    last_buttons = buttons;
    last_dx = dx;
    ++mouse_count;
    return true;
}

void hub_output_release_all(void) {
    ++release_count;
    memset(last_keyboard, 0, 8);
    last_buttons = 0;
}

static void reset(void) {
    memset(ready, 0, sizeof(ready));
    ready[0] = true;
    active = pending = 0;
    keyboard_count = mouse_count = release_count = select_count = 0;
    switching = fail_output = false;
    memset(last_keyboard, 0, 8);
    last_buttons = 0;
    last_dx = 0;
    capturing = finish_capture_on_report = false;
    captured_reports = 0;
    hub_router_init();
}

static void keyboard(unsigned input, uint8_t modifiers, uint8_t key) {
    uint8_t report[8] = {modifiers, 0, key};
    hub_router_keyboard(input, report);
}

static void finish_switch(void) {
    assert(switching);
    active = pending;
    switching = false;
    hub_router_poll();
}

static void test_normal_input(void) {
    reset();
    keyboard(0, 0, 4);
    assert(keyboard_count == 1 && last_keyboard[2] == 4);
    keyboard(0, 0, 0);
    assert(keyboard_count == 2 && last_keyboard[2] == 0);
    hub_router_mouse(1, 1, 12, -4, 1);
    assert(last_buttons == 1 && last_dx == 12);
    hub_router_mouse(1, 0, 0, 0, 0);
    assert(last_buttons == 0);
}

static void test_independent_sources(void) {
    reset();
    keyboard(0, 1, 4);
    keyboard(1, 2, 4);
    assert(last_keyboard[0] == 3 && last_keyboard[2] == 4 && last_keyboard[3] == 0);
    keyboard(0, 0, 0);
    assert(last_keyboard[0] == 2 && last_keyboard[2] == 4);
    hub_router_mouse(0, 1, 0, 0, 0);
    hub_router_mouse(1, 2, 0, 0, 0);
    assert(last_buttons == 3);
    hub_router_input_disconnected(0);
    assert(last_keyboard[0] == 2 && last_keyboard[2] == 4 && last_buttons == 2);
    hub_router_input_disconnected(1);
    assert(last_keyboard[0] == 0 && last_keyboard[2] == 0 && last_buttons == 0);
}

static void test_hotkey_skips_unready(void) {
    reset();
    ready[2] = true;
    keyboard(0, 5, 0);
    unsigned before = keyboard_count;
    keyboard(0, 5, 0x4f);
    assert(switching && pending == 2 && keyboard_count == before);
    keyboard(0, 5, 0x4f);
    assert(select_count == 1);
    finish_switch();
    keyboard(0, 5, 4);
    assert(keyboard_count == before);
    keyboard(0, 0, 0);
    keyboard(0, 0, 4);
    assert(keyboard_count == before + 1 && last_keyboard[0] == 0);
}

static void test_hotkey_wraps_left(void) {
    reset();
    ready[3] = true;
    keyboard(0, 0x50, 0x50);
    assert(switching && pending == 3);
}

static void test_no_other_computer(void) {
    reset();
    keyboard(0, 5, 0x4f);
    assert(!switching && select_count == 0 && release_count == 1);
    keyboard(0, 5, 0x4f);
    assert(release_count == 1);
    keyboard(0, 0, 0);
    keyboard(0, 0, 4);
    assert(keyboard_count == 1 && active == 0);
}

static void test_manual_switch_when_neutral(void) {
    reset();
    ready[1] = true;
    assert(hub_router_select(1));
    finish_switch();
    keyboard(0, 0, 4);
    assert(keyboard_count == 1 && last_keyboard[2] == 4);
}

static void test_switch_waits_for_both_sources(void) {
    reset();
    ready[1] = true;
    keyboard(0, 0, 4);
    hub_router_mouse(1, 1, 0, 0, 0);
    assert(hub_router_select(1));
    unsigned keys_before = keyboard_count;
    unsigned mouse_before = mouse_count;
    finish_switch();
    keyboard(0, 0, 0);
    keyboard(0, 0, 5);
    hub_router_mouse(1, 1, 10, 0, 0);
    assert(keyboard_count == keys_before && mouse_count == mouse_before);
    keyboard(0, 0, 0);
    hub_router_mouse(1, 0, 0, 0, 0);
    keyboard(0, 0, 6);
    assert(keyboard_count == keys_before + 1 && last_keyboard[2] == 6);
}

static void test_missing_output_does_not_replay(void) {
    reset();
    ready[0] = false;
    ready[1] = true;
    keyboard(0, 0, 4);
    assert(keyboard_count == 0 && release_count == 1);
    assert(hub_router_select(1));
    finish_switch();
    keyboard(0, 0, 4);
    assert(keyboard_count == 0);
    keyboard(0, 0, 0);
    keyboard(0, 0, 5);
    assert(keyboard_count == 1 && last_keyboard[2] == 5);
}

static void test_rollover_releases(void) {
    reset();
    const uint8_t six[8] = {0, 0, 4, 5, 6, 7, 8, 9};
    hub_router_keyboard(0, six);
    assert(keyboard_count == 1);
    keyboard(1, 0, 10);
    assert(release_count == 1 && keyboard_count == 1);
    keyboard(0, 0, 0);
    keyboard(1, 0, 0);
    keyboard(0, 0, 4);
    assert(keyboard_count == 2);
    keyboard(0, 0, 1);
    assert(release_count == 2);
}

static void test_overflow_recovery(void) {
    reset();
    fail_output = true;
    keyboard(0, 0, 4);
    assert(release_count == 1);
    fail_output = false;
    keyboard(0, 0, 4);
    assert(keyboard_count == 0);
    keyboard(0, 0, 0);
    keyboard(0, 0, 5);
    assert(keyboard_count == 1 && last_keyboard[2] == 5);
}

static void test_invalid_arguments(void) {
    reset();
    keyboard(HUB_INPUT_COUNT, 0, 4);
    hub_router_keyboard(0, NULL);
    hub_router_mouse(HUB_INPUT_COUNT, 1, 1, 1, 1);
    hub_router_input_disconnected(HUB_INPUT_COUNT);
    assert(!hub_router_select(HUB_COMPUTER_COUNT));
    assert(keyboard_count == 0 && mouse_count == 0 && select_count == 0);
}

static void test_stop_does_not_resume(void) {
    reset();
    hub_router_stop();
    hub_router_poll();
    keyboard(0, 0, 4);
    keyboard(0, 0, 0);
    hub_router_mouse(1, 0, 10, 0, 0);
    assert(keyboard_count == 0 && mouse_count == 0 && release_count == 1);
}

static void test_pairing_captures_before_routing(void) {
    reset();
    ready[1] = true;
    keyboard(0, 0, 4);
    capturing = true;
    hub_router_release_all();
    keyboard(0, 0, 0);
    keyboard(0, 0, 0x1e);
    hub_router_mouse(1, 1, 10, 0, 0);
    keyboard(0, 5, 0x4f);
    assert(captured_reports == 3 && keyboard_count == 1 && mouse_count == 0);
    assert(select_count == 0);
    capturing = false;
    keyboard(0, 0, 0);
    assert(keyboard_count == 1);
    hub_router_mouse(1, 0, 0, 0, 0);
    keyboard(0, 0, 5);
    assert(keyboard_count == 2 && last_keyboard[2] == 5);
}

static void test_pairing_without_a_ready_computer(void) {
    reset();
    ready[0] = false;
    keyboard(0, 0, 4);
    capturing = true;
    keyboard(0, 0, 0);
    keyboard(0, 0, 0x1e);
    assert(captured_reports == 2 && keyboard_count == 0);
    ready[0] = true;
    finish_capture_on_report = true;
    keyboard(0, 0, 0x28);
    assert(!capturing && keyboard_count == 0);
    keyboard(0, 0, 0);
    keyboard(0, 0, 4);
    assert(keyboard_count == 1 && last_keyboard[2] == 4);
}

static void test_input_first_does_not_replay_or_repeat_errors(void) {
    reset();
    ready[0] = false;
    hub_router_mouse(1, 0, 10, 0, 0);
    hub_router_mouse(1, 0, 20, 0, 0);
    hub_router_poll();
    hub_router_mouse(1, 0, 30, 0, 0);
    assert(release_count == 1 && mouse_count == 0);
    ready[0] = true;
    hub_router_poll();
    hub_router_mouse(1, 0, 5, 0, 0);
    assert(mouse_count == 1 && last_dx == 5);
}

static void test_new_input_requires_neutral_without_leaking_to_other_source(void) {
    reset();
    hub_router_input_connected(0);
    unsigned before = keyboard_count;
    keyboard(0, 1, 0x1e);
    assert(keyboard_count == before);
    keyboard(1, 0, 4);
    assert(last_keyboard[0] == 0 && last_keyboard[2] == 4 && last_keyboard[3] == 0);
    keyboard(0, 0, 0);
    keyboard(0, 0, 5);
    assert(last_keyboard[2] == 4 && last_keyboard[3] == 5);
    hub_router_mouse(0, 1, 40, 0, 0);
    hub_router_mouse(1, 2, 1, 0, 0);
    assert(last_buttons == 2 && last_dx == 1);
    hub_router_mouse(0, 0, 0, 0, 0);
    hub_router_mouse(0, 1, 2, 0, 0);
    assert(last_buttons == 3);
}

static void test_pairing_still_accepts_new_keyboard_before_normal_routing_is_armed(void) {
    reset();
    hub_router_input_connected(0);
    capturing = true;
    keyboard(0, 0, 0x1e);
    assert(captured_reports == 1);
}

static void test_switch_from_saved_offline_computer(void) {
    reset();
    ready[0] = false;
    ready[2] = true;
    keyboard(0, 0, 4);
    assert(keyboard_count == 0);
    keyboard(0, 0, 0);
    keyboard(0, 5, 0);
    keyboard(0, 5, 0x4f);
    assert(select_count == 1 && pending == 2 && switching);
    keyboard(0, 5, 0x4f);
    assert(select_count == 1 && keyboard_count == 0);
    finish_switch();
    keyboard(0, 0, 0);
    keyboard(0, 0, 5);
    assert(keyboard_count == 1 && last_keyboard[2] == 5);
}

int main(void) {
    test_normal_input();
    test_independent_sources();
    test_hotkey_skips_unready();
    test_hotkey_wraps_left();
    test_no_other_computer();
    test_manual_switch_when_neutral();
    test_switch_waits_for_both_sources();
    test_missing_output_does_not_replay();
    test_rollover_releases();
    test_overflow_recovery();
    test_invalid_arguments();
    test_stop_does_not_resume();
    test_pairing_captures_before_routing();
    test_pairing_without_a_ready_computer();
    test_input_first_does_not_replay_or_repeat_errors();
    test_new_input_requires_neutral_without_leaking_to_other_source();
    test_pairing_still_accepts_new_keyboard_before_normal_routing_is_armed();
    test_switch_from_saved_offline_computer();
    puts("All router tests passed.");
    return 0;
}
