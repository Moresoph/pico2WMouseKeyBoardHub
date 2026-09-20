#include "hub_router.h"

#include <stdio.h>
#include <string.h>

#include "hub_config.h"
#include "hub_output.h"
#include "hub_security.h"

typedef struct {
    uint8_t keyboard[8];
    uint8_t buttons;
    bool keyboard_armed;
    bool mouse_armed;
} input_state_t;

static input_state_t inputs[HUB_INPUT_COUNT];
static bool suppressed;
static bool stopped;

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

static bool combined_keyboard(uint8_t report[8]) {
    bool pressed[256] = {false};
    memset(report, 0, 8);
    unsigned count = 0;
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        if (!inputs[i].keyboard_armed) continue;
        report[0] |= inputs[i].keyboard[0];
        for (unsigned j = 2; j < 8; ++j) {
            uint8_t key = inputs[i].keyboard[j];
            if (key > 0 && key < 4) return false;
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

static void suppress_with_release(const char *reason) {
    if (!suppressed) printf("[route] %s; release physical keys/buttons to resume\n", reason);
    suppressed = true;
    hub_output_release_all();
}

static bool should_suppress(void) {
    if (stopped) return true;
    if (hub_security_capture_active()) {
        suppressed = true;
        return true;
    }
    if (!suppressed && !hub_output_switching()) return false;
    suppressed = true;
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

void hub_router_init(void) {
    memset(inputs, 0, sizeof(inputs));
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        inputs[i].keyboard_armed = true;
        inputs[i].mouse_armed = true;
    }
    suppressed = false;
    stopped = false;
}

void hub_router_poll(void) {
    if (!stopped && suppressed && !hub_output_switching()
        && !hub_security_capture_active() && hub_output_ready(hub_output_active_slot())
        && all_released()) {
        suppressed = false;
    }
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
    return true;
}

void hub_router_keyboard(unsigned input, const uint8_t report[8]) {
    if (input >= HUB_INPUT_COUNT || report == NULL) {
        printf("[route] invalid keyboard source/report\n");
        return;
    }
    hub_router_poll();
    memcpy(inputs[input].keyboard, report, 8);
    if (!stopped && hub_security_keyboard(input, report)) {
        suppressed = true;
        return;
    }
    if (!inputs[input].keyboard_armed) {
        bool neutral = report[0] == 0;
        for (unsigned i = 2; i < 8; ++i) neutral &= report[i] == 0;
        inputs[input].keyboard_armed = neutral;
        return;
    }
    if (should_suppress()) return;

    uint8_t merged[8];
    if (!combined_keyboard(merged)) {
        suppress_with_release("keyboard rollover (six non-modifier keys maximum)");
        return;
    }

    bool ctrl_alt = (merged[0] & 0x11u) && (merged[0] & 0x44u);
    bool right = has_key(merged, 0x4f);
    bool left = has_key(merged, 0x50);
    if (ctrl_alt && (right || left)) {
        /* Modifier presses may already have reached the old computer. */
        if (right == left || !switch_relative(right ? 1 : -1)) {
            suppress_with_release("switch chord consumed");
        } else {
            suppressed = true;
        }
        return;
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
    hub_router_poll();
    inputs[input].buttons = buttons & 0x1fu;
    if (!inputs[input].mouse_armed) {
        inputs[input].mouse_armed = inputs[input].buttons == 0;
        return;
    }
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
    if (!combined_keyboard(merged)
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
    suppress_with_release("manual release");
}

void hub_router_stop(void) {
    stopped = true;
    suppress_with_release("routing stopped");
}

void hub_router_status(void) {
    printf("[route] selected=%u switching=%u suppressed=%u physical-neutral=%u\n",
           hub_output_active_slot() + 1, hub_output_switching(), suppressed,
           all_released());
}
