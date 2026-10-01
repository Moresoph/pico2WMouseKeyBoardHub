#ifndef HUB_USB_WIRE_H
#define HUB_USB_WIRE_H

#include <stdint.h>

void hub_usb_wire_setup(uint8_t address, const uint8_t setup[8]);
void hub_usb_wire_poll(void);
void hub_usb_wire_status(void);
void hub_usb_wire_dump(void);
void hub_usb_wire_arm(void);

#endif
