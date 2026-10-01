#ifndef HUB_USB_TEST_STRUCTS_USB_H
#define HUB_USB_TEST_STRUCTS_USB_H
#include <stdint.h>
#include "usb_registers.h"
typedef struct { volatile uint32_t phy_direct, phy_direct_override, sie_ctrl; } usb_hw_t;
extern usb_hw_t *usb_hw;
#endif
