#ifndef HUB_BUTTON_H
#define HUB_BUTTON_H

#include <stdbool.h>
#include <stdint.h>

#define HUB_BUTTON_DEBOUNCE_MS 50u
#define HUB_BUTTON_RESET_MS 8000u

typedef enum {
    HUB_BUTTON_NONE,
    HUB_BUTTON_ENROLL,
    HUB_BUTTON_RESET_INPUTS
} hub_button_action_t;

typedef struct {
    bool initialized;
    bool raw_pressed;
    bool stable_pressed;
    bool armed;
    bool reset_sent;
    uint32_t raw_since;
    uint32_t pressed_since;
} hub_button_t;

void hub_button_init(hub_button_t *button);
hub_button_action_t hub_button_tick(hub_button_t *button, bool pressed, uint32_t now);
bool hub_bootsel_pressed(void);

#endif
