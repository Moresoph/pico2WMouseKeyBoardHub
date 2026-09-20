#include "hub_passkey.h"
#include <string.h>

static bool neutral(const hub_passkey_t *state) {
    for (unsigned input = 0; input < HUB_INPUT_COUNT; ++input) {
        const uint8_t *report = state->previous[input];
        if (report[0]) return false;
        for (unsigned i = 2; i < 8; ++i) {
            if (report[i]) return false;
        }
    }
    return true;
}

static bool contains(const uint8_t report[8], uint8_t key) {
    for (unsigned i = 2; i < 8; ++i) {
        if (report[i] == key) return true;
    }
    return false;
}

static void erase_digits(hub_passkey_t *state) {
    memset(state->digits, 0, sizeof(state->digits));
    state->count = 0;
}

void hub_passkey_init(hub_passkey_t *state) {
    memset(state, 0, sizeof(*state));
    state->source = HUB_INPUT_COUNT;
}

bool hub_passkey_begin(hub_passkey_t *state) {
    if (state->active) return false;
    erase_digits(state);
    state->active = true;
    state->submitted = false;
    state->source = HUB_INPUT_COUNT;
    state->armed = neutral(state);
    return true;
}

void hub_passkey_end(hub_passkey_t *state) {
    erase_digits(state);
    state->active = false;
    state->submitted = false;
    state->armed = false;
    state->source = HUB_INPUT_COUNT;
}

void hub_passkey_submitted(hub_passkey_t *state) {
    erase_digits(state);
    state->submitted = true;
}

bool hub_passkey_disconnected(hub_passkey_t *state, unsigned input) {
    if (input >= HUB_INPUT_COUNT) return false;
    memset(state->previous[input], 0, 8);
    return state->active && state->source == input;
}

static hub_passkey_event_t invalid_input(hub_passkey_t *state) {
    erase_digits(state);
    state->source = HUB_INPUT_COUNT;
    state->armed = false;
    return HUB_PASSKEY_INVALID;
}

hub_passkey_event_t hub_passkey_feed(hub_passkey_t *state, unsigned input,
                                   const uint8_t report[8], uint32_t *value) {
    if (input >= HUB_INPUT_COUNT || report == NULL || value == NULL) {
        return HUB_PASSKEY_INVALID;
    }
    uint8_t previous[8];
    memcpy(previous, state->previous[input], 8);
    memcpy(state->previous[input], report, 8);
    if (!state->active || state->submitted) return HUB_PASSKEY_NONE;
    if (!state->armed) {
        state->armed = neutral(state);
        return HUB_PASSKEY_NONE;
    }
    if (state->source < HUB_INPUT_COUNT && input != state->source) {
        return HUB_PASSKEY_NONE;
    }

    uint8_t key = 0;
    unsigned fresh = 0;
    for (unsigned i = 2; i < 8; ++i) {
        uint8_t current = report[i];
        if (current && current < 4) return invalid_input(state);
        if (!current || contains(previous, current)) continue;
        bool duplicate = false;
        for (unsigned j = 2; j < i; ++j) duplicate |= report[j] == current;
        if (duplicate) continue;
        if (current == 0x29) return HUB_PASSKEY_CANCEL;
        key = current;
        ++fresh;
    }
    if (!fresh) return HUB_PASSKEY_NONE;
    /* A HID report does not encode the order of simultaneous new key presses. */
    if (fresh != 1 || report[0]) return invalid_input(state);
    if (key == 0x2a) {
        if (state->count) state->digits[--state->count] = 0;
        return HUB_PASSKEY_EDITED;
    }
    if (key == 0x28 || key == 0x58) {
        if (state->count != 6) return HUB_PASSKEY_INCOMPLETE;
        *value = 0;
        for (unsigned i = 0; i < 6; ++i) *value = *value * 10u + state->digits[i];
        hub_passkey_submitted(state);
        return HUB_PASSKEY_SUBMIT;
    }
    unsigned digit;
    if (key >= 0x1e && key <= 0x26) digit = key - 0x1d;
    else if (key == 0x27 || key == 0x62) digit = 0;
    else if (key >= 0x59 && key <= 0x61) digit = key - 0x58;
    else return invalid_input(state);
    if (state->count == 6) return invalid_input(state);
    state->source = input;
    state->digits[state->count++] = (uint8_t)digit;
    return HUB_PASSKEY_EDITED;
}
