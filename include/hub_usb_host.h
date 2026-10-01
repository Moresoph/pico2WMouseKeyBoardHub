#ifndef HUB_USB_HOST_H
#define HUB_USB_HOST_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t rx_timeouts, sie_ctrl, sie_status, ints, inte, address_control;
    uint32_t buffer_control, remaining, transferred, linestate_tuning;
    uint16_t packet_size;
    uint8_t address, endpoint;
    bool active, hub_timing_supported, hub_timing_enabled;
} hub_usb_hcd_state_t;

bool hub_usb_host_init(void);
void hub_usb_host_status(void);
void hub_usb_hcd_get_state(hub_usb_hcd_state_t *state);
void hub_usb_enum_poll(void);
void hub_usb_enum_status(void);
bool hub_usb_enum_can_rearm(void);
void hub_usb_enum_block(uint8_t hub, uint8_t port, const char *error);
void hub_usb_enum_cancel_power_wait(void);
void hub_usb_hub_poll(void);
bool hub_usb_hub_power_pending(void);

#endif
