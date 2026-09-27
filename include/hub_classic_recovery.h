#ifndef HUB_CLASSIC_RECOVERY_H
#define HUB_CLASSIC_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>
#include "btstack_defines.h"

/* Run from the input timer, never from inside an HCI/L2CAP callback. */
uint8_t hub_classic_hid_recover(uint16_t cid, const uint8_t address[6]);

/* Private interfaces between the pinned SDK overlays. */
bool hub_classic_l2cap_reap(const uint8_t address[6], btstack_packet_handler_t owner);
bool hub_classic_sdp_retire(const uint8_t address[6], btstack_packet_handler_t owner);

#endif
