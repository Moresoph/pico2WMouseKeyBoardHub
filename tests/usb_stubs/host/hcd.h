#ifndef HUB_TEST_HCD_H
#define HUB_TEST_HCD_H
#include <stdbool.h>
#include <stdint.h>
typedef struct { uint8_t rhport, hub_addr, hub_port, speed; } hcd_devtree_info_t;
void hcd_devtree_get_info(uint8_t address, hcd_devtree_info_t *info);
bool hcd_port_connect_status(uint8_t port);
uint32_t hcd_frame_number(uint8_t port);
#endif
