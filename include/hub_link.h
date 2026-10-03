#ifndef HUB_LINK_H
#define HUB_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HUB_LINK_SLOTS 2u
#define HUB_LINK_PAYLOAD 46u
#define HUB_LINK_OUTPUT_PAYLOAD 76u
#define HUB_LINK_FRAME_PAYLOAD HUB_LINK_OUTPUT_PAYLOAD
#define HUB_LINK_RAW_MAX (15u + HUB_LINK_FRAME_PAYLOAD)
#define HUB_LINK_WIRE_MAX (2u * HUB_LINK_RAW_MAX + 2u)
#define HUB_LINK_REQUEST 1u
#define HUB_LINK_STATE 2u
#define HUB_LINK_OUTPUT 3u
#define HUB_LINK_PC_SLOTS 7u
#define HUB_LINK_UI 4u
#define HUB_LINK_MANAGE 5u
#define HUB_LINK_MANAGE_REPLY 6u
#define HUB_LINK_OUTPUT_PERIOD_MS 1000u
#define HUB_LINK_OUTPUT_TIMEOUT_MS 3000u
#define HUB_LINK_PC_RESERVED 1u
#define HUB_LINK_PC_CONNECTED 2u
#define HUB_LINK_PC_ENCRYPTED 4u
#define HUB_LINK_PC_READY 8u
#define HUB_LINK_PC_SUSPENDED 16u
#define HUB_LINK_OUTPUT_SWITCHING 1u
#define HUB_LINK_OUTPUT_MOUSE_BUFFER 2u
#define HUB_LINK_OUTPUT_INPUT_ONLINE 4u
#define HUB_LINK_OUTPUT_STORAGE_FAULT 8u
#define HUB_LINK_OUTPUT_PAIRING 16u
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
    uint8_t payload[HUB_LINK_FRAME_PAYLOAD];
} hub_link_frame_t;

typedef struct {
    uint8_t bytes[HUB_LINK_RAW_MAX];
    unsigned length;
    bool started, escaped, discard;
    uint32_t errors;
} hub_link_parser_t;

typedef struct {
    uint8_t flags, address_type, address[6];
    uint16_t interval;
} hub_link_pc_t;

typedef struct {
    uint8_t selected, flags, version[3];
    hub_link_pc_t computers[HUB_LINK_PC_SLOTS];
} hub_link_output_t;

typedef struct {
    bool valid;
    uint32_t nonce, session, received_at;
    uint16_t sequence;
    hub_link_output_t value;
} hub_link_output_cache_t;

size_t hub_link_encode(const hub_link_frame_t *frame, uint8_t wire[HUB_LINK_WIRE_MAX]);
bool hub_link_feed(hub_link_parser_t *parser, uint8_t byte, hub_link_frame_t *frame);
void hub_link_pack(hub_link_frame_t *frame, const hub_link_input_t inputs[HUB_LINK_SLOTS]);
bool hub_link_unpack(const hub_link_frame_t *frame, hub_link_input_t inputs[HUB_LINK_SLOTS]);
void hub_link_pack_output(hub_link_frame_t *frame, const hub_link_output_t *output);
bool hub_link_unpack_output(const hub_link_frame_t *frame, hub_link_output_t *output);
bool hub_link_receive_output(hub_link_output_cache_t *cache, const hub_link_frame_t *frame,
                             uint32_t nonce, uint32_t session, uint32_t now);
bool hub_link_output_fresh(const hub_link_output_cache_t *cache,
                           uint32_t nonce, uint32_t session, uint32_t now);

#endif
