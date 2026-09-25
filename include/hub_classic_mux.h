#ifndef HUB_CLASSIC_MUX_H
#define HUB_CLASSIC_MUX_H

#include <stdbool.h>
#include <stdint.h>
#include "btstack_defines.h"
#include "gap.h"

/* Init before either HID profile; start after both, before powering on HCI. */
void hub_classic_mux_init(void);
bool hub_classic_mux_start(void);
bool hub_classic_mux_output_ready(uint16_t handle);
bool hub_classic_mux_output_pending(void);
/* packet includes HIDP framing. Returns the checked L2CAP status. */
uint8_t hub_classic_mux_send(const uint8_t *packet, uint16_t size);

/* Private integration points used only by the pinned, generated HID sources. */
uint8_t hub_classic_mux_register_host(btstack_packet_handler_t callback,
                                    uint16_t psm, uint16_t mtu, gap_security_level_t level);
uint8_t hub_classic_mux_register_device(btstack_packet_handler_t callback,
                                      uint16_t psm, uint16_t mtu, gap_security_level_t level);
void hub_classic_mux_decline(uint16_t cid);

#endif
