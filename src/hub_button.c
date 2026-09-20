#include "hub_button.h"
#include <string.h>

void hub_button_init(hub_button_t *button) {
    memset(button, 0, sizeof(*button));
}

hub_button_action_t hub_button_tick(hub_button_t *button, bool pressed, uint32_t now) {
    if (!button->initialized) {
        button->initialized = true;
        button->raw_pressed = pressed;
        button->stable_pressed = true;
        button->raw_since = now;
        /* Require a released button before accepting gestures after boot. */
        return HUB_BUTTON_NONE;
    }
    if (pressed != button->raw_pressed) {
        button->raw_pressed = pressed;
        button->raw_since = now;
    }
    if (pressed != button->stable_pressed
        && (uint32_t)(now - button->raw_since) >= HUB_BUTTON_DEBOUNCE_MS) {
        button->stable_pressed = pressed;
        if (!pressed) {
            bool short_press = button->armed && !button->reset_sent;
            button->armed = true;
            button->reset_sent = false;
            return short_press ? HUB_BUTTON_ENROLL : HUB_BUTTON_NONE;
        }
        button->pressed_since = now;
        button->reset_sent = false;
    }
    if (button->armed && button->stable_pressed && button->raw_pressed
        && !button->reset_sent
        && (uint32_t)(now - button->pressed_since) >= HUB_BUTTON_RESET_MS) {
        button->reset_sent = true;
        return HUB_BUTTON_RESET_INPUTS;
    }
    return HUB_BUTTON_NONE;
}
