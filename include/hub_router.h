#ifndef HUB_ROUTER_H
#define HUB_ROUTER_H

#include <stdbool.h>
#include <stdint.h>

void hub_router_init(void);
void hub_router_poll(void);
void hub_router_keyboard(unsigned input, const uint8_t report[8]);
void hub_router_mouse(unsigned input, uint8_t buttons, int32_t dx, int32_t dy,
                      int32_t wheel);
void hub_router_input_disconnected(unsigned input);
void hub_router_input_connected(unsigned input);
bool hub_router_select(unsigned slot);
void hub_router_release_all(void);
void hub_router_stop(void);
void hub_router_status(void);

#endif
