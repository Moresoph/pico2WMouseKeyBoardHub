#ifndef HUB_UI_H
#define HUB_UI_H
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef enum { HUB_UI_NONE, HUB_UI_SHORT, HUB_UI_DOUBLE, HUB_UI_LONG } hub_ui_action_t;
typedef struct {
    bool initialized, raw, stable, armed, long_press, waiting;
    uint32_t changed, pressed, released;
} hub_ui_button_t;

static inline hub_ui_action_t hub_ui_tick(hub_ui_button_t *b, bool pressed, uint32_t now) {
    if (!b->initialized) {
        b->initialized = true; b->raw = pressed; b->stable = pressed;
        b->armed = !pressed; b->changed = now;
        return HUB_UI_NONE;
    }
    hub_ui_action_t action = HUB_UI_NONE;
    if (b->waiting && !b->stable && now - b->released >= 350) {
        b->waiting = false; action = HUB_UI_SHORT;
    }
    if (pressed != b->raw) { b->raw = pressed; b->changed = now; }
    if (b->raw != b->stable && now - b->changed >= 50) {
        b->stable = b->raw;
        if (b->stable) { b->pressed = now; b->long_press = false; }
        else if (!b->armed) b->armed = true;
        else if (b->long_press || now - b->pressed >= 3000) {
            b->waiting = false; action = HUB_UI_LONG;
        } else if (b->waiting && now - b->released < 350) {
            b->waiting = false; action = HUB_UI_DOUBLE;
        } else { b->waiting = true; b->released = now; }
    }
    if (b->armed && b->stable && now - b->pressed >= 3000) {
        b->long_press = true; b->waiting = false;
    }
    return action;
}
typedef enum { HUB_LED_OFF, HUB_LED_RECONNECT, HUB_LED_PAIR, HUB_LED_CODE, HUB_LED_FAULT, HUB_LED_ACK } hub_ui_led_t;
static inline bool hub_ui_led(hub_ui_led_t mode, uint32_t now) {
    switch (mode) {
        case HUB_LED_RECONNECT: return now % 2000 < 100;
        case HUB_LED_PAIR: return now % 1000 < 500;
        case HUB_LED_CODE: return now % 200 < 100;
        case HUB_LED_FAULT: return now % 4000 < 2000 || now % 200 < 100;
        case HUB_LED_ACK: return true;
        default: return false;
    }
}
typedef struct {
    bool active;
    uint32_t started;
} hub_ui_fault_clock_t;

static inline bool hub_ui_fault_tick(hub_ui_fault_clock_t *clock, bool active, uint32_t now) {
    if (!active) { clock->active = false; return false; }
    if (!clock->active) { clock->active = true; clock->started = now; }
    return hub_ui_led(HUB_LED_FAULT, now - clock->started);
}
#endif
