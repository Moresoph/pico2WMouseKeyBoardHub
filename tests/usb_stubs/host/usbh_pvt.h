#ifndef HUB_TEST_USBH_PVT_H
#define HUB_TEST_USBH_PVT_H
#include "tusb.h"

typedef struct {
    const char *name;
    bool (*init)(void);
    bool (*deinit)(void);
    bool (*open)(uint8_t, uint8_t, const tusb_desc_interface_t *, uint16_t);
    bool (*set_config)(uint8_t, uint8_t);
    bool (*xfer_cb)(uint8_t, uint8_t, xfer_result_t, uint32_t);
    void (*close)(uint8_t);
} usbh_class_driver_t;

#endif
