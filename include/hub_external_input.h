#ifndef HUB_EXTERNAL_INPUT_H
#define HUB_EXTERNAL_INPUT_H

#include "hub_link.h"

typedef struct {
    uint32_t nonce, session, last_rx, accepted, resets;
    uint16_t sequence;
    bool online;
    hub_link_input_t inputs[HUB_LINK_SLOTS];
} hub_external_input_t;

void hub_external_reset(hub_external_input_t *state, uint32_t nonce);
bool hub_external_receive(hub_external_input_t *state, const hub_link_frame_t *frame, uint32_t now);
bool hub_external_expired(const hub_external_input_t *state, uint32_t now);

#endif
