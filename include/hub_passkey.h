#ifndef HUB_PASSKEY_H
#define HUB_PASSKEY_H

#include <stdbool.h>
#include <stdint.h>
#include "hub_config.h"

typedef enum {
    HUB_PASSKEY_NONE,
    HUB_PASSKEY_EDITED,
    HUB_PASSKEY_SUBMIT,
    HUB_PASSKEY_CANCEL,
    HUB_PASSKEY_INCOMPLETE,
    HUB_PASSKEY_INVALID
} hub_passkey_event_t;

typedef struct {
    bool active;
    bool submitted;
    bool armed;
    unsigned source;
    unsigned count;
    uint8_t digits[6];
    uint8_t previous[HUB_INPUT_COUNT][8];
} hub_passkey_t;

void hub_passkey_init(hub_passkey_t *state);
bool hub_passkey_begin(hub_passkey_t *state);
void hub_passkey_end(hub_passkey_t *state);
void hub_passkey_submitted(hub_passkey_t *state);
bool hub_passkey_disconnected(hub_passkey_t *state, unsigned input);
hub_passkey_event_t hub_passkey_feed(hub_passkey_t *state, unsigned input,
                                   const uint8_t report[8], uint32_t *value);

#endif
