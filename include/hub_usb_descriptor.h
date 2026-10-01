#ifndef HUB_USB_DESCRIPTOR_H
#define HUB_USB_DESCRIPTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t number, alternate, class_code, subclass, protocol;
    uint8_t event_in, acl_in, acl_out;
    uint16_t event_size, acl_in_size, acl_out_size;
    bool bluetooth_hci;
} hub_usb_interface_t;

/* Inspect alternate setting zero without interpreting SCO/audio alternatives. */
bool hub_usb_inspect_interface(const uint8_t *data, size_t length, hub_usb_interface_t *result);

#endif
