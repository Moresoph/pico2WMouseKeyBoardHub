#include "hub_router.h"

#include <stdio.h>
#include <string.h>

#include "hub_config.h"
#include "hub_latency.h"
#include "hub_output.h"
#include "hub_security.h"

#define MOUSE_SWITCH_HOLD_US 800000u

typedef struct {
    uint8_t keyboard[8];
    uint8_t buttons;
    bool keyboard_armed;
    bool escape_pending;
    bool mouse_armed;
    bool mouse_chord_pending;
    bool mouse_chord_blocked;
    uint32_t mouse_chord_started_us;
} input_state_t;

static input_state_t inputs[HUB_INPUT_COUNT];
static bool suppressed;
static bool stopped;
static bool switch_chord_blocked;
static bool direct_chord_active;

static bool all_released(void) {
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        if (inputs[i].buttons || inputs[i].keyboard[0]) return false;
        for (unsigned j = 2; j < 8; ++j) {
            if (inputs[i].keyboard[j]) return false;
        }
    }
    return true;
}

static uint8_t combined_buttons(void) {
    uint8_t buttons = 0;
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        if (inputs[i].mouse_armed) buttons |= inputs[i].buttons;
    }
    return buttons;
}

static bool combined_keyboard(uint8_t report[8], bool include_escape) {
    bool pressed[256] = {false};
    memset(report, 0, 8);
    unsigned count = 0;
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        if (!inputs[i].keyboard_armed) continue;
        report[0] |= inputs[i].keyboard[0];
        for (unsigned j = 2; j < 8; ++j) {
            uint8_t key = inputs[i].keyboard[j];
            if (key > 0 && key < 4) return false;
            if (key == 0x29 && !include_escape) continue;
            if (key >= 4 && !pressed[key]) {
                pressed[key] = true;
                ++count;
            }
        }
    }
    if (count > 6) return false;
    unsigned index = 2;
    for (unsigned key = 4; key < 256; ++key) {
        if (pressed[key]) report[index++] = (uint8_t)key;
    }
    return true;
}

static bool has_key(const uint8_t report[8], uint8_t key) {
    for (unsigned i = 2; i < 8; ++i) {
        if (report[i] == key) return true;
    }
    return false;
}

static void cancel_mouse_chords(void) {
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        inputs[i].mouse_chord_pending = false;
        if (inputs[i].buttons & 0x03u) inputs[i].mouse_chord_blocked = true;
    }
}

static void cancel_deferred_escapes(void) {
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) inputs[i].escape_pending = false;
}

static void suppress_with_release(const char *reason) {
    if (!suppressed) printf("[route] %s; release physical keys/buttons to resume\n", reason);
    suppressed = true;
    cancel_deferred_escapes();
    hub_output_release_all();
}

static bool should_suppress(void) {
    if (stopped) return true;
    if (hub_security_capture_active()) {
        suppressed = true;
        cancel_deferred_escapes();
        cancel_mouse_chords();
        return true;
    }
    if (!suppressed && !hub_output_switching()) return false;
    suppressed = true;
    cancel_deferred_escapes();
    if (!hub_output_switching() && hub_output_ready(hub_output_active_slot())
        && all_released()) suppressed = false;
    return true;
}

static bool switch_relative(int step) {
    int current = (int)hub_output_active_slot();
    for (int distance = 1; distance < HUB_COMPUTER_COUNT; ++distance) {
        unsigned slot = (unsigned)((current + step * distance + HUB_COMPUTER_COUNT)
                                   % HUB_COMPUTER_COUNT);
        if (hub_output_ready(slot)) return hub_router_select(slot);
    }
    printf("[route] no other ready computer; selection unchanged\n");
    return false;
}

static bool mouse_switch_allowed(void) {
    return !stopped && !hub_security_capture_active() && !hub_output_switching()
        && !direct_chord_active
        && (!suppressed || !hub_output_ready(hub_output_active_slot()));
}

static void poll_mouse_chords(void) {
    if (!mouse_switch_allowed()) {
        cancel_mouse_chords();
        return;
    }
    uint32_t now = hub_latency_now_us();
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        input_state_t *input = &inputs[i];
        if (!input->mouse_chord_pending
            || (uint32_t)(now - input->mouse_chord_started_us) <= MOUSE_SWITCH_HOLD_US)
            continue;
        input->mouse_chord_pending = false;
        input->mouse_chord_blocked = true;
        printf("[route] mouse %u left+right held over 0.8 seconds; next ready computer\n", i + 1);
        if (!switch_relative(1)) suppress_with_release("mouse switch chord consumed");
        return;
    }
}

void hub_router_init(void) {
    memset(inputs, 0, sizeof(inputs));
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        inputs[i].keyboard_armed = true;
        inputs[i].mouse_armed = true;
    }
    suppressed = false;
    stopped = false;
    switch_chord_blocked = false;
    direct_chord_active = false;
}

static void resume_routing(void) {
    if (stopped || suppressed || hub_output_switching()
        || hub_security_capture_active() || !hub_output_ready(hub_output_active_slot())) {
        cancel_deferred_escapes();
    }
    if (all_released()) {
        switch_chord_blocked = false;
        direct_chord_active = false;
    }
    if (!stopped && suppressed && !hub_output_switching()
        && !hub_security_capture_active() && hub_output_ready(hub_output_active_slot())
        && all_released()) {
        suppressed = false;
    }
}

void hub_router_poll(void) {
    resume_routing();
    poll_mouse_chords();
}

bool hub_router_select(unsigned slot) {
    if (slot >= HUB_COMPUTER_COUNT) {
        printf("[route] invalid computer slot\n");
        return false;
    }
    if (slot == hub_output_active_slot() && hub_output_ready(slot)
        && !hub_output_switching()) return true;
    if (!hub_output_select(slot)) return false;
    suppressed = true;
    cancel_deferred_escapes();
    cancel_mouse_chords();
    return true;
}

void hub_router_keyboard(unsigned input, const uint8_t report[8]) {
    if (input >= HUB_INPUT_COUNT || report == NULL) {
        printf("[route] invalid keyboard source/report\n");
        return;
    }
    resume_routing();
    bool escape_was_down = has_key(inputs[input].keyboard, 0x29);
    memcpy(inputs[input].keyboard, report, 8);
    if (!stopped && hub_security_keyboard(input, report)) {
        suppressed = true;
        cancel_deferred_escapes();
        cancel_mouse_chords();
        return;
    }
    if (!inputs[input].keyboard_armed) {
        bool neutral = report[0] == 0;
        for (unsigned i = 2; i < 8; ++i) neutral &= report[i] == 0;
        inputs[input].keyboard_armed = neutral;
        return;
    }
    if (stopped || hub_security_capture_active() || hub_output_switching()) {
        should_suppress();
        return;
    }

    uint8_t merged[8];
    if (!combined_keyboard(merged, true)) {
        suppress_with_release("keyboard rollover (six non-modifier keys maximum)");
        return;
    }

    bool ctrl_alt = (merged[0] & 0x11u) && (merged[0] & 0x44u);
    bool right = has_key(merged, 0x4f);
    bool left = has_key(merged, 0x50);
    if (has_key(merged, 0x29) && !switch_chord_blocked
        && (!suppressed || !hub_output_ready(hub_output_active_slot()))) {
        unsigned target = 0, numbers = 0;
        for (unsigned number = 1; number <= 9; ++number) {
            if (has_key(merged, (uint8_t)(0x1d + number))) {
                target = number;
                ++numbers;
            }
            if (has_key(merged, (uint8_t)(0x58 + number))) {
                target = number;
                ++numbers;
            }
        }
        if (has_key(merged, 0x27)) ++numbers;
        if (has_key(merged, 0x62)) ++numbers;
        if (numbers) {
            direct_chord_active = true;
            switch_chord_blocked = true;
            cancel_mouse_chords();
            cancel_deferred_escapes();
            bool switch_accepted = false;
            /* release_all makes ready peers temporarily unready; select first. */
            if (numbers != 1 || !target || target > HUB_COMPUTER_COUNT) {
                printf("[route] press one PC digit (1-%u); selection unchanged\n",
                       HUB_COMPUTER_COUNT);
            } else if (!hub_output_ready(target - 1)) {
                printf("[route] Esc+%u: PC%u offline/not ready; still PC%u\n",
                       target, target, hub_output_active_slot() + 1);
            } else if (target - 1 == hub_output_active_slot()) {
                printf("[route] Esc+%u: PC%u already selected\n", target, target);
            } else if (hub_router_select(target - 1)) {
                switch_accepted = true;
                printf("[route] Esc+%u: switch to PC%u accepted\n", target, target);
            } else {
                printf("[route] Esc+%u: switch to PC%u rejected; release all keys/buttons\n",
                       target, target);
            }
            if (!switch_accepted) suppress_with_release("Escape+digit selection chord consumed");
            return;
        }
    }
    if (direct_chord_active) {
        switch_chord_blocked = true;
        return;
    }
    if (ctrl_alt && (right || left) && !switch_chord_blocked
        && (!suppressed || !hub_output_ready(hub_output_active_slot()))) {
        switch_chord_blocked = true;
        /* Modifier presses may already have reached the old computer. */
        if (right == left || !switch_relative(right ? 1 : -1)) {
            suppress_with_release("switch chord consumed");
        } else {
            suppressed = true;
        }
        return;
    }
    if (should_suppress()) return;
    bool escape_down = has_key(report, 0x29);
    bool send_escape = inputs[input].escape_pending && !escape_down;
    if (escape_down && !escape_was_down) inputs[input].escape_pending = true;
    if (!escape_down) inputs[input].escape_pending = false;
    if (!combined_keyboard(merged, false)) {
        suppress_with_release("keyboard rollover (six non-modifier keys maximum)");
        return;
    }
    if (send_escape) {
        if (merged[7]) {
            suppress_with_release("deferred Escape exceeds six-key output capacity");
            return;
        }
        uint8_t tap[8];
        memcpy(tap, merged, sizeof(tap));
        for (unsigned i = 2; i < 8; ++i) {
            if (!tap[i]) {
                tap[i] = 0x29;
                break;
            }
        }
        if (!hub_output_keyboard(tap)) {
            suppress_with_release("deferred Escape output unavailable");
            return;
        }
    }
    if (!hub_output_keyboard(merged)) {
        suppress_with_release("keyboard output unavailable");
    }
}

void hub_router_mouse(unsigned input, uint8_t buttons, int32_t dx, int32_t dy,
                      int32_t wheel) {
    if (input >= HUB_INPUT_COUNT) {
        printf("[route] invalid mouse source\n");
        return;
    }
    resume_routing();
    inputs[input].buttons = buttons & 0x1fu;
    if (!inputs[input].mouse_armed) {
        inputs[input].mouse_armed = inputs[input].buttons == 0;
        return;
    }
    input_state_t *state = &inputs[input];
    uint8_t chord = state->buttons & 0x03u;
    if (!chord) {
        state->mouse_chord_pending = false;
        state->mouse_chord_blocked = false;
    } else if (!mouse_switch_allowed()) {
        state->mouse_chord_pending = false;
        state->mouse_chord_blocked = true;
    } else if (chord != 0x03u) {
        state->mouse_chord_pending = false;
    } else if (!state->mouse_chord_blocked && !state->mouse_chord_pending) {
        state->mouse_chord_started_us = hub_latency_now_us();
        state->mouse_chord_pending = true;
    }
    poll_mouse_chords();
    if (should_suppress()) return;
    if (!hub_output_mouse(combined_buttons(), dx, dy, wheel)) {
        suppress_with_release("mouse output unavailable");
    }
}

void hub_router_input_disconnected(unsigned input) {
    if (input >= HUB_INPUT_COUNT) {
        printf("[route] invalid disconnected source\n");
        return;
    }
    memset(&inputs[input], 0, sizeof(inputs[input]));
    hub_security_input_disconnected(input);
    if (should_suppress()) return;
    uint8_t merged[8];
    if (!combined_keyboard(merged, false)
        || !hub_output_keyboard(merged)
        || !hub_output_mouse(combined_buttons(), 0, 0, 0)) {
        suppress_with_release("source disconnected; clearing output");
    }
}

void hub_router_input_connected(unsigned input) {
    if (input >= HUB_INPUT_COUNT) {
        printf("[route] invalid connected source\n");
        return;
    }
    hub_router_input_disconnected(input);
    printf("[route] input %u connected; release its keys/buttons before forwarding\n", input + 1);
}

void hub_router_release_all(void) {
    cancel_mouse_chords();
    suppress_with_release("manual release");
}

void hub_router_stop(void) {
    stopped = true;
    cancel_mouse_chords();
    suppress_with_release("routing stopped");
}

void hub_router_status(void) {
    printf("[route] selected=%u switching=%u suppressed=%u physical-neutral=%u\n",
           hub_output_active_slot() + 1, hub_output_switching(), suppressed,
           all_released());
}
