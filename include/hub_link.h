#ifndef HUB_LINK_H
#define HUB_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HUB_LINK_SLOTS 2u
#define HUB_LINK_PAYLOAD 46u
#define HUB_LINK_RAW_MAX 61u
#define HUB_LINK_WIRE_MAX (2u * HUB_LINK_RAW_MAX + 2u)
#define HUB_LINK_REQUEST 1u
#define HUB_LINK_STATE 2u
#define HUB_LINK_KEYBOARD 1u
#define HUB_LINK_MOUSE 2u
#define HUB_LINK_TIMEOUT_MS 500u
#define HUB_LINK_HEARTBEAT_MS 100u

typedef struct {
    uint8_t epoch, roles, keyboard[8], buttons;
    int32_t dx, dy, wheel;
} hub_link_input_t;

typedef struct {
    uint8_t type, length;
    uint32_t nonce, session;
    uint16_t sequence;
    uint8_t payload[HUB_LINK_PAYLOAD];
} hub_link_frame_t;

typedef struct {
    uint8_t bytes[HUB_LINK_RAW_MAX];
    unsigned length;
    bool started, escaped, discard;
    uint32_t errors;
} hub_link_parser_t;

size_t hub_link_encode(const hub_link_frame_t *frame, uint8_t wire[HUB_LINK_WIRE_MAX]);
bool hub_link_feed(hub_link_parser_t *parser, uint8_t byte, hub_link_frame_t *frame);
void hub_link_pack(hub_link_frame_t *frame, const hub_link_input_t inputs[HUB_LINK_SLOTS]);
bool hub_link_unpack(const hub_link_frame_t *frame, hub_link_input_t inputs[HUB_LINK_SLOTS]);

#endif
