#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hub_passkey.h"

static hub_passkey_t state;
static uint32_t value;

static hub_passkey_event_t key(unsigned input, uint8_t usage) {
    uint8_t report[8] = {0, 0, usage};
    hub_passkey_event_t event = hub_passkey_feed(&state, input, report, &value);
    memset(report, 0, sizeof(report));
    hub_passkey_feed(&state, input, report, &value);
    return event;
}

static void start(void) {
    hub_passkey_init(&state);
    value = UINT32_MAX;
    assert(hub_passkey_begin(&state));
}

static void digits(unsigned input, const char *text) {
    for (unsigned i = 0; text[i]; ++i) {
        uint8_t usage = text[i] == '0' ? 0x27 : (uint8_t)(0x1d + text[i] - '0');
        assert(key(input, usage) == HUB_PASSKEY_EDITED);
    }
}

static void assert_digits_erased(void) {
    assert(state.count == 0);
    for (unsigned i = 0; i < sizeof(state.digits); ++i) assert(state.digits[i] == 0);
}

static void test_six_digits_and_leading_zeros(void) {
    start();
    digits(0, "001234");
    assert(value == UINT32_MAX);
    assert(key(0, 0x28) == HUB_PASSKEY_SUBMIT && value == 1234);
    assert(state.active && state.submitted);
    assert_digits_erased();
    assert(key(0, 0x28) == HUB_PASSKEY_NONE);
    hub_passkey_end(&state);
    assert(!state.active);
    start();
    digits(0, "000000");
    assert(key(0, 0x28) == HUB_PASSKEY_SUBMIT && value == 0);
}

static void test_keypad_and_backspace(void) {
    start();
    assert(key(1, 0x59) == HUB_PASSKEY_EDITED);
    assert(key(1, 0x5a) == HUB_PASSKEY_EDITED);
    assert(key(1, 0x2a) == HUB_PASSKEY_EDITED && state.count == 1);
    assert(key(1, 0x62) == HUB_PASSKEY_EDITED);
    digits(1, "3456");
    assert(key(1, 0x58) == HUB_PASSKEY_SUBMIT && value == 103456);
}

static void test_repeat_and_source_isolation(void) {
    start();
    uint8_t report[8] = {0, 0, 0x1e};
    assert(hub_passkey_feed(&state, 0, report, &value) == HUB_PASSKEY_EDITED);
    assert(hub_passkey_feed(&state, 0, report, &value) == HUB_PASSKEY_NONE);
    assert(state.count == 1);
    assert(key(1, 0x1f) == HUB_PASSKEY_NONE && state.count == 1);
    memset(report, 0, sizeof(report));
    hub_passkey_feed(&state, 0, report, &value);
    assert(key(0, 0x1e) == HUB_PASSKEY_EDITED && state.count == 2);
}

static void test_incomplete_and_overflow(void) {
    start();
    digits(0, "123");
    assert(key(0, 0x28) == HUB_PASSKEY_INCOMPLETE && state.count == 3);
    digits(0, "456");
    assert(key(0, 0x24) == HUB_PASSKEY_INVALID);
    assert_digits_erased();
    assert(value == UINT32_MAX);
    digits(0, "654321");
    assert(key(0, 0x28) == HUB_PASSKEY_SUBMIT && value == 654321);
}

static void test_held_key_at_start(void) {
    hub_passkey_init(&state);
    uint8_t report[8] = {0, 0, 0x1e};
    hub_passkey_feed(&state, 0, report, &value);
    assert(hub_passkey_begin(&state) && !state.armed);
    assert(!hub_passkey_begin(&state));
    assert(hub_passkey_feed(&state, 0, report, &value) == HUB_PASSKEY_NONE);
    memset(report, 0, sizeof(report));
    hub_passkey_feed(&state, 0, report, &value);
    assert(state.armed && state.count == 0);
    assert(key(0, 0x1e) == HUB_PASSKEY_EDITED && state.count == 1);
}

static void test_invalid_and_ambiguous_reports(void) {
    start();
    digits(0, "12");
    uint8_t report[8] = {0, 0, 0x20, 0x21};
    assert(hub_passkey_feed(&state, 0, report, &value) == HUB_PASSKEY_INVALID);
    assert_digits_erased();
    assert(hub_passkey_feed(&state, 0, report, &value) == HUB_PASSKEY_NONE);
    memset(report, 0, sizeof(report));
    hub_passkey_feed(&state, 0, report, &value);
    report[0] = 2;
    report[2] = 0x1e;
    assert(hub_passkey_feed(&state, 0, report, &value) == HUB_PASSKEY_INVALID);
    assert_digits_erased();
    memset(report, 0, sizeof(report));
    hub_passkey_feed(&state, 0, report, &value);
    assert(key(0, 1) == HUB_PASSKEY_INVALID);
    assert(key(0, 4) == HUB_PASSKEY_INVALID);
}

static void test_cancel_disconnect_and_console_submission(void) {
    start();
    digits(0, "123");
    assert(key(0, 0x29) == HUB_PASSKEY_CANCEL && value == UINT32_MAX);
    hub_passkey_end(&state);
    assert_digits_erased();
    assert(!state.active);
    start();
    digits(1, "42");
    assert(!hub_passkey_disconnected(&state, 0));
    assert(hub_passkey_disconnected(&state, 1));
    hub_passkey_end(&state);
    assert_digits_erased();
    start();
    digits(0, "123");
    hub_passkey_submitted(&state);
    assert(state.active && state.submitted);
    assert_digits_erased();
    assert(key(0, 0x28) == HUB_PASSKEY_NONE);
}

int main(void) {
    test_six_digits_and_leading_zeros();
    test_keypad_and_backspace();
    test_repeat_and_source_isolation();
    test_incomplete_and_overflow();
    test_held_key_at_start();
    test_invalid_and_ambiguous_reports();
    test_cancel_disconnect_and_console_submission();
    puts("All passkey tests passed.");
    return 0;
}
