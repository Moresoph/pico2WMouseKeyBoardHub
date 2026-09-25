#ifndef HUB_CLASSIC_OUTPUT_H
#define HUB_CLASSIC_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

bool hub_classic_output_set_target(const uint8_t address[6]);
bool hub_classic_output_is_peer(const uint8_t address[6]);
bool hub_classic_output_allow_pairing(const uint8_t address[6]);
bool hub_classic_output_accept_connection(const uint8_t address[6]);
void hub_classic_output_pairing_changed(bool enabled);
/* Incoming-only experiment: explains the limitation and returns false. */
bool hub_classic_output_connect(void);

#endif
