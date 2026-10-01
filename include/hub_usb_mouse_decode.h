#ifndef HUB_USB_MOUSE_DECODE_H
#define HUB_USB_MOUSE_DECODE_H

#include <stdbool.h>
#include <stdint.h>

#define HUB_USB_MOUSE_FIELDS 32u
typedef struct {
    uint16_t bit;
    uint8_t id, size, axis, button;
} hub_usb_mouse_field_t;
typedef struct {
    hub_usb_mouse_field_t fields[HUB_USB_MOUSE_FIELDS];
    uint8_t sizes[256];
    bool mouse[256], numbered, valid;
    unsigned count;
} hub_usb_mouse_layout_t;
typedef struct {
    int32_t dx, dy, wheel;
    uint8_t buttons;
} hub_usb_mouse_motion_t;

bool hub_usb_mouse_layout(hub_usb_mouse_layout_t *layout, const uint8_t *descriptor, uint16_t length);
bool hub_usb_mouse_decode(const hub_usb_mouse_layout_t *layout, const uint8_t *report,
                          uint16_t length, hub_usb_mouse_motion_t *motion);

#endif
