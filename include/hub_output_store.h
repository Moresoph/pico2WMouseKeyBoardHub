#ifndef HUB_OUTPUT_STORE_H
#define HUB_OUTPUT_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include "hub_config.h"

#define HUB_OUTPUT_STORE_SLOTS 4u
#if HUB_CLASSIC_OUTPUT_TEST
#define HUB_OUTPUT_STORE_TAG 0x50484331u /* PHC1; never replace the BLE PC record. */
#define HUB_OUTPUT_STORE_NAME "PHC1"
#else
#define HUB_OUTPUT_STORE_TAG 0x50484f31u /* PHO1; separate from input policy/bond tags. */
#define HUB_OUTPUT_STORE_NAME "PHO1"
#endif
#define HUB_OUTPUT_STORE_BYTES 44u

typedef struct {
    bool reserved;
    uint8_t type;
    uint8_t address[6];
} hub_output_identity_t;

typedef struct {
    hub_output_identity_t identities[HUB_OUTPUT_STORE_SLOTS];
    unsigned selected;
    bool loaded;
    bool fault;
} hub_output_store_t;

bool hub_output_identity_valid(uint8_t type, const uint8_t address[6]);
bool hub_output_store_load(hub_output_store_t *store);
int hub_output_store_find(const hub_output_store_t *store, uint8_t type,
                          const uint8_t address[6]);
int hub_output_store_claim(hub_output_store_t *store, uint8_t type,
                           const uint8_t address[6]);
bool hub_output_store_select(hub_output_store_t *store, unsigned slot);

#endif
