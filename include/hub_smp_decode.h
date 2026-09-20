#ifndef HUB_SMP_DECODE_H
#define HUB_SMP_DECODE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t handle;
    uint8_t code;
    uint8_t io;
    uint8_t oob;
    uint8_t auth;
    uint8_t key_size;
    uint8_t initiator_keys;
    uint8_t responder_keys;
    uint8_t reason;
} hub_smp_metadata_t;

bool hub_smp_decode(const uint8_t *packet, uint16_t length, hub_smp_metadata_t *metadata);
void hub_smp_trace_init(void);
void hub_pairing_note(const char *action, uint32_t ticket, int result);
void hub_pairing_diagnostics(void);

#endif
