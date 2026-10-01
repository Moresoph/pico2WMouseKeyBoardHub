#ifndef HUB_TEST_TUSB_H
#define HUB_TEST_TUSB_H

#include <stdbool.h>
#include <stdint.h>
#include "tusb_config.h"

/* Transport boundary only; firmware builds use the real SDK TinyUSB headers. */
typedef enum { TUSB_SPEED_FULL, TUSB_SPEED_LOW, TUSB_SPEED_HIGH } tusb_speed_t;
typedef enum { TUSB_ROLE_INVALID, TUSB_ROLE_DEVICE, TUSB_ROLE_HOST } tusb_role_t;
typedef enum { XFER_RESULT_SUCCESS, XFER_RESULT_FAILED, XFER_RESULT_STALLED } xfer_result_t;
typedef struct { tusb_role_t role; tusb_speed_t speed; } tusb_rhport_init_t;
typedef struct {
    uint8_t bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting;
    uint8_t bNumEndpoints, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface;
} tusb_desc_interface_t;

bool tusb_rhport_init(uint8_t port, const tusb_rhport_init_t *config);
#define tusb_init tusb_rhport_init
bool tuh_vid_pid_get(uint8_t address, uint16_t *vid, uint16_t *pid);
bool tuh_mounted(uint8_t address);
tusb_speed_t tuh_speed_get(uint8_t address);
void tuh_task_ext(uint32_t timeout, bool in_isr);
bool tuh_task_event_ready(void);
#define HID_PROTOCOL_REPORT 1
void tuh_hid_set_default_protocol(uint8_t protocol);
bool tuh_hid_mounted(uint8_t address, uint8_t index);
bool tuh_hid_receive_report(uint8_t address, uint8_t index);
uint8_t tuh_hid_interface_protocol(uint8_t address, uint8_t index);

#endif
