#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "hub_config.h"
#include "hub_latency.h"
#include "hub_output.h"
#include "hub_router.h"
#include "hub_security.h"

static bool ready[HUB_COMPUTER_COUNT];
static bool switching;
static bool fail_output;
static bool fail_selection;
static unsigned active;
static unsigned pending;
static unsigned keyboard_count;
static unsigned mouse_count;
static unsigned release_count;
static unsigned select_count;
static uint8_t last_keyboard[8];
static uint8_t keyboard_history[512][8];
static unsigned fail_keyboard_at;
static uint8_t last_buttons;
static int32_t last_dx;
static bool capturing;
static bool finish_capture_on_report;
static unsigned captured_reports;
static uint32_t clock_us;

uint32_t hub_latency_now_us(void) { return clock_us; }

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
    if (!hub_output_ready(slot) || switching || fail_selection) return false;
    pending = slot;
    switching = true;
    ++select_count;
    return true;
}

bool hub_output_keyboard(const uint8_t report[8]) {
    if (fail_output || switching || !ready[active]
        || (fail_keyboard_at && keyboard_count + 1 == fail_keyboard_at)) return false;
    assert(keyboard_count < sizeof(keyboard_history) / sizeof(keyboard_history[0]));
    memcpy(keyboard_history[keyboard_count], report, 8);
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
    fail_keyboard_at = 0;
    memset(keyboard_history, 0, sizeof(keyboard_history));
    switching = fail_output = false;
    fail_selection = false;
    memset(last_keyboard, 0, 8);
    last_buttons = 0;
    last_dx = 0;
    capturing = finish_capture_on_report = false;
    captured_reports = 0;
    clock_us = 0;
    hub_router_init();
}

static void keyboard(unsigned input, uint8_t modifiers, uint8_t key) {
    uint8_t report[8] = {modifiers, 0, key};
    hub_router_keyboard(input, report);
}

static void finish_switch(void) {
    assert(switching);
    memset(last_keyboard, 0, sizeof(last_keyboard));
    last_buttons = 0;
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

static void esc_digit(unsigned input, uint8_t digit, uint8_t extra) {
    const uint8_t report[8] = {0, 0, 0x29, digit, extra};
    hub_router_keyboard(input, report);
}

static void test_direct_fixed_pc_numbers(void) {
    for (unsigned keypad = 0; keypad < 2; ++keypad) {
        for (unsigned number = 1; number <= HUB_COMPUTER_COUNT; ++number) {
            reset();
            for (unsigned slot = 0; slot < HUB_COMPUTER_COUNT; ++slot) ready[slot] = true;
            active = number % HUB_COMPUTER_COUNT;
            uint8_t digit = (uint8_t)((keypad ? 0x58 : 0x1d) + number);
            esc_digit(0, digit, 0);
            assert(switching && pending == number - 1 && select_count == 1);
            assert(keyboard_count == 0 && release_count == 0);
            finish_switch();
            esc_digit(0, digit, 0);
            assert(select_count == 1 && keyboard_count == 0);
            keyboard(0, 0, 0);
            keyboard(0, 0, 4);
            assert(keyboard_count == 1 && last_keyboard[2] == 4);
        }
    }
    reset();
    ready[0] = false;
    ready[1] = ready[HUB_COMPUTER_COUNT - 1] = true;
    esc_digit(0, 0x1f, 0);
    assert(switching && pending == 1);
}

static void test_direct_chord_staging_and_same_target(void) {
    reset();
    ready[2] = true;
    keyboard(0, 0, 0x29);
    assert(last_keyboard[2] == 0 && !switching && release_count == 0);
    esc_digit(0, 0x20, 0);
    assert(switching && pending == 2 && keyboard_count == 1);
    assert(release_count == 0 && last_keyboard[2] == 0);

    reset();
    esc_digit(0, 0x1e, 0);
    assert(!switching && select_count == 0 && release_count == 1);
    keyboard(0, 0, 0x1e);
    assert(keyboard_count == 0);
    keyboard(0, 0, 0);
    keyboard(0, 0, 4);
    assert(keyboard_count == 1);
}

static void test_direct_missing_ambiguous_and_cancelled(void) {
    reset();
    ready[2] = true;
    esc_digit(0, 0x1f, 0);
    assert(!switching && release_count == 1 && keyboard_count == 0);
    ready[1] = true;
    esc_digit(0, 0x1f, 0);
    assert(!switching);
    keyboard(0, 0, 0);
    esc_digit(0, 0x1e, 0x1f);
    assert(!switching && release_count == 2);
    esc_digit(0, 0x1f, 0);
    assert(!switching);
    keyboard(0, 0, 0);
    esc_digit(0, 0x20, 0x5b);
    assert(!switching && release_count == 3);
    keyboard(0, 0, 0);
    esc_digit(0, 0x27, 0);
    assert(!switching && release_count == 4);
    keyboard(0, 0, 0);
    esc_digit(0, 0x25, 0);
    assert(!switching && release_count == 5);
    keyboard(0, 0, 0);
    esc_digit(0, 0x26, 0);
    assert(!switching && release_count == 6);
    keyboard(0, 0, 0);
    esc_digit(0, 0x5b, 0);
    assert(switching && pending == 2);
}

static void test_escape_digits_and_old_arrows_still_forward(void) {
    reset();
    ready[2] = true;
    keyboard(0, 0, 0x29);
    assert(keyboard_count == 1 && last_keyboard[2] == 0 && release_count == 0);
    keyboard(0, 0, 0);
    assert(keyboard_count == 3 && keyboard_history[1][2] == 0x29
        && keyboard_history[2][2] == 0);
    keyboard(0, 0, 0x20);
    assert(last_keyboard[2] == 0x20 && !switching);
    esc_digit(0, 0x20, 0);
    assert(switching && pending == 2);
    finish_switch();
    assert(last_keyboard[2] == 0);

    reset();
    ready[2] = true;
    const uint8_t old_chord[8] = {0, 0, 0x50, 0x4f, 0x1f};
    hub_router_keyboard(0, old_chord);
    assert(!switching && keyboard_count == 1 && release_count == 0);
    assert(last_keyboard[2] == 0x1f && last_keyboard[3] == 0x4f
        && last_keyboard[4] == 0x50);
    keyboard(0, 0, 0);
    esc_digit(0, 0, 4);
    assert(!switching && last_keyboard[2] == 4 && last_keyboard[3] == 0);

    reset();
    ready[1] = true;
    hub_router_input_connected(0);
    esc_digit(0, 0x1f, 0);
    assert(!switching);
    keyboard(0, 0, 0);
    esc_digit(0, 0x1f, 0);
    assert(switching && pending == 1);
}

static void test_mouse_hold_boundary_and_no_repeat(void) {
    reset();
    ready[2] = true;
    hub_router_mouse(1, 3, 0, 0, 0);
    assert(last_buttons == 3 && !switching);
    clock_us = 400000;
    hub_router_mouse(1, 3, 10, 0, 0);
    clock_us = 799999;
    hub_router_poll();
    assert(!switching);
    clock_us = 800000;
    hub_router_poll();
    assert(!switching);
    ++clock_us;
    hub_router_poll();
    assert(switching && pending == 2 && select_count == 1);
    finish_switch();
    clock_us += 10000000;
    hub_router_mouse(1, 3, 10, 0, 0);
    hub_router_poll();
    assert(select_count == 1);
    hub_router_mouse(1, 0, 0, 0, 0);
    hub_router_poll();
    hub_router_mouse(1, 3, 0, 0, 0);
    clock_us += 800001;
    hub_router_poll();
    assert(switching && pending == 0 && select_count == 2);
}

static void test_mouse_hold_cancel_and_release_at_deadline(void) {
    reset();
    ready[1] = true;
    hub_router_mouse(1, 3, 0, 0, 0);
    clock_us = 800001;
    hub_router_mouse(1, 0, 0, 0, 0);
    assert(!switching);
    hub_router_mouse(1, 3, 0, 0, 0);
    clock_us += 799999;
    hub_router_mouse(1, 1, 0, 0, 0);
    clock_us += 800001;
    hub_router_poll();
    assert(!switching);
    hub_router_mouse(1, 3, 0, 0, 0);
    clock_us += 800001;
    hub_router_mouse(1, 3, 10, 0, 0);
    assert(switching && select_count == 1);
}

static void test_mouse_source_isolation_and_rollover(void) {
    reset();
    ready[1] = true;
    hub_router_mouse(0, 1, 0, 0, 0);
    hub_router_mouse(1, 2, 0, 0, 0);
    clock_us = 3000000;
    hub_router_poll();
    assert(!switching);
    hub_router_mouse(0, 0, 0, 0, 0);
    hub_router_mouse(1, 0, 0, 0, 0);
    clock_us = UINT32_MAX - 400000u;
    hub_router_mouse(1, 7, 0, 0, 0);
    clock_us += 800000u;
    hub_router_poll();
    assert(!switching);
    ++clock_us;
    hub_router_poll();
    assert(switching && pending == 1);
}

static void test_mouse_no_target_and_offline_selection(void) {
    reset();
    hub_router_mouse(1, 3, 0, 0, 0);
    clock_us = 800001;
    hub_router_poll();
    assert(!switching && select_count == 0 && release_count == 1);
    ready[2] = true;
    clock_us += 10000000;
    hub_router_mouse(1, 3, 0, 0, 0);
    hub_router_poll();
    assert(!switching && release_count == 1);

    reset();
    ready[0] = false;
    ready[2] = true;
    hub_router_mouse(1, 3, 0, 0, 0);
    clock_us = 800001;
    hub_router_poll();
    assert(switching && pending == 2);
}

static void test_new_chords_respect_capture_stop_and_rearm(void) {
    reset();
    ready[1] = true;
    hub_router_mouse(1, 3, 0, 0, 0);
    capturing = true;
    clock_us = 3000000;
    hub_router_poll();
    esc_digit(0, 0x1f, 0);
    assert(!switching && captured_reports == 1);
    capturing = false;
    keyboard(0, 0, 0);
    hub_router_mouse(1, 3, 0, 0, 0);
    clock_us += 3000000;
    hub_router_poll();
    assert(!switching);
    hub_router_mouse(1, 0, 0, 0, 0);
    hub_router_poll();
    hub_router_mouse(1, 3, 0, 0, 0);
    hub_router_stop();
    clock_us += 3000000;
    hub_router_poll();
    esc_digit(0, 0x1f, 0);
    assert(!switching);

    reset();
    ready[1] = true;
    hub_router_input_connected(1);
    hub_router_mouse(1, 3, 0, 0, 0);
    clock_us = 3000000;
    hub_router_poll();
    assert(!switching);
    hub_router_mouse(1, 0, 0, 0, 0);
    hub_router_mouse(1, 3, 0, 0, 0);
    hub_router_input_disconnected(1);
    clock_us += 3000000;
    hub_router_poll();
    assert(!switching);
}

static void test_keyboard_chord_cancels_mouse_timer(void) {
    reset();
    ready[1] = ready[2] = true;
    hub_router_mouse(1, 3, 0, 0, 0);
    clock_us = 400000;
    esc_digit(0, 0x1f, 0);
    assert(switching && pending == 1);
    finish_switch();
    keyboard(0, 0, 0);
    clock_us += 3000000;
    hub_router_poll();
    assert(select_count == 1);
    hub_router_mouse(1, 0, 0, 0, 0);
    keyboard(0, 0, 4);
    assert(keyboard_count == 1);
}

static void test_rejected_selection_and_manual_release(void) {
    reset();
    ready[1] = true;
    fail_selection = true;
    esc_digit(0, 0x1f, 0);
    assert(!switching && active == 0 && release_count == 1);
    fail_selection = false;
    esc_digit(0, 0x1f, 0);
    assert(!switching);

    reset();
    ready[0] = false;
    ready[1] = true;
    hub_router_mouse(1, 3, 0, 0, 0);
    hub_router_release_all();
    clock_us = 3000000;
    hub_router_poll();
    hub_router_mouse(1, 3, 0, 0, 0);
    assert(!switching);
    hub_router_mouse(1, 0, 0, 0, 0);
    hub_router_mouse(1, 3, 0, 0, 0);
    capturing = finish_capture_on_report = true;
    keyboard(0, 0, 0x28);
    assert(!capturing);
    clock_us += 3000000;
    hub_router_poll();
    assert(!switching);
}

static bool report_has_escape(const uint8_t report[8]) {
    for (unsigned i = 2; i < 8; ++i) {
        if (report[i] == 0x29) return true;
    }
    return false;
}

static unsigned escape_reports(void) {
    unsigned count = 0;
    for (unsigned i = 0; i < keyboard_count; ++i)
        count += report_has_escape(keyboard_history[i]);
    return count;
}

static void test_deferred_escape_repeats_and_physical_release(void) {
    reset();
    keyboard(0, 0, 0x29);
    for (unsigned repeat = 0; repeat < 5; ++repeat) {
        hub_router_poll();
        keyboard(0, 0, 0x29);
    }
    assert(escape_reports() == 0);
    unsigned before = keyboard_count;
    keyboard(0, 0, 0);
    assert(keyboard_count == before + 2);
    assert(keyboard_history[before][2] == 0x29
        && !memcmp(keyboard_history[before + 1], (uint8_t[8]){0}, 8));
    keyboard(0, 0, 0);
    assert(escape_reports() == 1);
    keyboard(0, 0, 0x29);
    keyboard(0, 0, 0);
    assert(escape_reports() == 2);
}

static void test_deferred_escape_preserves_other_keys_and_modifiers(void) {
    reset();
    esc_digit(0, 0, 4);
    keyboard(1, 2, 5);
    assert(escape_reports() == 0);
    unsigned before = keyboard_count;
    keyboard(0, 1, 4);
    const uint8_t down[8] = {3, 0, 4, 5, 0x29};
    const uint8_t up[8] = {3, 0, 4, 5};
    assert(keyboard_count == before + 2);
    assert(!memcmp(keyboard_history[before], down, 8));
    assert(!memcmp(keyboard_history[before + 1], up, 8));
}

static void test_deferred_escape_shortcut_results_never_replay(void) {
    for (unsigned result = 0; result < 7; ++result) {
        reset();
        ready[1] = true;
        keyboard(0, 0, 0x29);
        uint8_t digit = 0x1f, extra = 0;
        if (result == 1) digit = 0x1e; /* Already selected. */
        if (result == 2) ready[1] = false; /* Offline. */
        if (result == 3) digit = 0x25; /* Invalid slot. */
        if (result == 4) extra = 0x1e; /* Ambiguous digits. */
        if (result == 5) fail_selection = true;
        if (result == 6) {
            keyboard(0, 0, 0);
            reset(); /* Simultaneous Esc+digit, not Esc-first. */
            ready[1] = true;
        }
        esc_digit(0, digit, extra);
        if (switching) finish_switch();
        keyboard(0, 0, 0x29); /* Digit released first. */
        keyboard(0, 0, 0);
        hub_router_poll();
        keyboard(0, 0, 4);
        assert(escape_reports() == 0 && last_keyboard[2] == 4);
    }

    reset();
    ready[1] = true;
    esc_digit(0, 0x1f, 0);
    finish_switch();
    keyboard(0, 0, 0x1f); /* Escape released first. */
    keyboard(0, 0, 0);
    keyboard(0, 0, 4);
    assert(escape_reports() == 0 && last_keyboard[2] == 4);
}

static void test_deferred_escape_cancelled_on_safety_boundaries(void) {
    for (unsigned boundary = 0; boundary < 7; ++boundary) {
        reset();
        ready[1] = true;
        keyboard(0, 0, 0x29);
        if (boundary == 0) hub_router_input_disconnected(0);
        if (boundary == 1) hub_router_release_all();
        if (boundary == 2) {
            assert(hub_router_select(1));
            finish_switch();
        }
        if (boundary == 3) {
            capturing = true;
            hub_router_poll();
            capturing = false;
        }
        if (boundary == 4) {
            ready[0] = false;
            hub_router_poll();
            ready[0] = true;
        }
        if (boundary == 5) {
            keyboard(0, 0, 1); /* Rollover. */
        }
        if (boundary == 6) {
            capturing = finish_capture_on_report = true;
            keyboard(0, 0, 0x29);
            assert(!capturing);
        }
        keyboard(0, 0, 0);
        hub_router_poll();
        keyboard(0, 0, 4);
        assert(escape_reports() == 0 && last_keyboard[2] == 4);
    }
    reset();
    keyboard(0, 0, 0x29);
    hub_router_stop();
    keyboard(0, 0, 0);
    assert(escape_reports() == 0);
}

static void test_deferred_escape_independent_sources_and_disconnect_filter(void) {
    reset();
    keyboard(0, 0, 0x29);
    keyboard(1, 0, 0x29);
    hub_router_input_disconnected(1);
    assert(escape_reports() == 0);
    keyboard(0, 0, 0);
    assert(escape_reports() == 1);

    reset();
    keyboard(0, 0, 0x29);
    keyboard(1, 0, 0x29);
    keyboard(0, 0, 0);
    assert(escape_reports() == 1 && !report_has_escape(last_keyboard));
    keyboard(1, 0, 0);
    assert(escape_reports() == 2 && !report_has_escape(last_keyboard));
}

static void test_deferred_escape_output_failure_and_capacity(void) {
    for (unsigned phase = 1; phase <= 2; ++phase) {
        reset();
        keyboard(0, 0, 0x29);
        fail_keyboard_at = keyboard_count + phase;
        keyboard(0, 0, 0);
        assert(release_count == 1 && !report_has_escape(last_keyboard));
        fail_keyboard_at = 0;
        keyboard(0, 0, 0);
        keyboard(0, 0, 4);
        assert(last_keyboard[2] == 4 && escape_reports() == phase - 1);
    }
    reset();
    keyboard(0, 0, 0x29);
    const uint8_t six[8] = {0, 0, 4, 5, 6, 7, 8, 9};
    hub_router_keyboard(0, six);
    assert(release_count == 1 && escape_reports() == 0);
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
    test_direct_fixed_pc_numbers();
    test_direct_chord_staging_and_same_target();
    test_direct_missing_ambiguous_and_cancelled();
    test_escape_digits_and_old_arrows_still_forward();
    test_mouse_hold_boundary_and_no_repeat();
    test_mouse_hold_cancel_and_release_at_deadline();
    test_mouse_source_isolation_and_rollover();
    test_mouse_no_target_and_offline_selection();
    test_new_chords_respect_capture_stop_and_rearm();
    test_keyboard_chord_cancels_mouse_timer();
    test_rejected_selection_and_manual_release();
    test_deferred_escape_repeats_and_physical_release();
    test_deferred_escape_preserves_other_keys_and_modifiers();
    test_deferred_escape_shortcut_results_never_replay();
    test_deferred_escape_cancelled_on_safety_boundaries();
    test_deferred_escape_independent_sources_and_disconnect_filter();
    test_deferred_escape_output_failure_and_capacity();
    puts("All router tests passed.");
    return 0;
}
