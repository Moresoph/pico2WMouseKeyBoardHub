#ifndef HUB_CLASSIC_DECODE_H
#define HUB_CLASSIC_DECODE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    HUB_CLASSIC_PIN_REQUEST,
    HUB_CLASSIC_PIN_REPLY,
    HUB_CLASSIC_PIN_NEGATIVE,
    HUB_CLASSIC_PIN_ACK,
    HUB_CLASSIC_AUTH_COMPLETE,
    HUB_CLASSIC_ENCRYPTION,
    HUB_CLASSIC_KEY_REQUEST,
    HUB_CLASSIC_KEY_CREATED,
    HUB_CLASSIC_NAME,
    HUB_CLASSIC_DISCONNECTED,
    HUB_CLASSIC_AUTH_REQUEST,
    HUB_CLASSIC_ENCRYPTION_REQUEST,
    HUB_CLASSIC_DISCONNECT_REQUEST,
    HUB_CLASSIC_L2CAP_REQUEST,
    HUB_CLASSIC_L2CAP_RESPONSE
} hub_classic_event_t;

typedef struct {
    hub_classic_event_t event;
    uint16_t handle;
    uint16_t opcode;
    uint8_t address[6];
    uint8_t status;
    uint8_t value;
    uint16_t psm;
    uint16_t source_cid;
    uint16_t destination_cid;
    uint16_t result;
    uint16_t l2cap_status;
    char name[101];
} hub_classic_metadata_t;

bool hub_classic_command_decode(const uint8_t *packet, uint16_t length, hub_classic_metadata_t *out);
bool hub_classic_event_decode(const uint8_t *packet, uint16_t length, hub_classic_metadata_t *out);
bool hub_classic_l2cap_next(const uint8_t *packet, uint16_t length, unsigned *offset,
                           hub_classic_metadata_t *out);

#endif
