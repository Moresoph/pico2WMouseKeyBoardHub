#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hub_usb_descriptor.h"

static const uint8_t bluetooth[] = {
    9, 4, 0, 0, 3, 0xe0, 1, 1, 0,
    7, 5, 0x81, 3, 16, 0, 1,
    7, 5, 0x82, 2, 64, 0, 0,
    7, 5, 0x02, 2, 64, 0, 0,
    9, 4, 1, 0, 0, 0xe0, 1, 1, 0
};

int main(void) {
    hub_usb_interface_t info;
    assert(hub_usb_inspect_interface(bluetooth, sizeof(bluetooth), &info));
    assert(info.bluetooth_hci && info.event_in == 0x81 && info.event_size == 16);
    assert(info.acl_in == 0x82 && info.acl_out == 2 && info.acl_in_size == 64 && info.acl_out_size == 64);
    for (unsigned length = 0; length < 30; ++length)
        assert(!hub_usb_inspect_interface(bluetooth, length, &info));
    assert(!hub_usb_inspect_interface(NULL, 30, &info));
    assert(!hub_usb_inspect_interface(bluetooth, 30, NULL));
    uint8_t data[sizeof(bluetooth)];
    memcpy(data, bluetooth, sizeof(data));
    data[9] = 0;
    assert(!hub_usb_inspect_interface(data, 30, &info));
    data[9] = 255;
    assert(!hub_usb_inspect_interface(data, 30, &info));
    memcpy(data, bluetooth, sizeof(data));
    data[25] = 0x82;
    assert(!hub_usb_inspect_interface(data, 30, &info)); /* Repeated endpoint address. */
    data[25] = 0;
    assert(!hub_usb_inspect_interface(data, 30, &info)); /* EP0 is not an interface endpoint. */
    memcpy(data, bluetooth, sizeof(data));
    data[5] = 0xff;
    assert(hub_usb_inspect_interface(data, 30, &info) && !info.bluetooth_hci);
    data[5] = 0xe0;
    data[3] = 1;
    assert(hub_usb_inspect_interface(data, 30, &info) && !info.bluetooth_hci);
    data[3] = 0;
    data[20] = 0x00; data[21] = 0x02;
    assert(hub_usb_inspect_interface(data, 30, &info) && !info.bluetooth_hci); /* HS-only bulk size. */
    memcpy(data, bluetooth, sizeof(data));
    data[4] = 2;
    assert(!hub_usb_inspect_interface(data, 30, &info));
    data[4] = 3;
    data[19] = 3;
    assert(hub_usb_inspect_interface(data, 30, &info) && !info.bluetooth_hci); /* Multiple interrupt inputs. */
    const uint8_t hub[] = {9,4,0,0,1,9,0,0,0, 7,5,0x81,3,1,0,12};
    assert(hub_usb_inspect_interface(hub, sizeof(hub), &info) && !info.bluetooth_hci && info.class_code == 9);
    const uint8_t sco[] = {9,4,1,0,0,0xe0,1,1,0};
    assert(hub_usb_inspect_interface(sco, sizeof(sco), &info) && !info.bluetooth_hci);
    puts("USB interface descriptors: bounds, alternate settings, endpoint validation and HCI candidates passed");
    return 0;
}
