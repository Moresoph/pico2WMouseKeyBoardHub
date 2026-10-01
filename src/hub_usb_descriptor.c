#include <string.h>
#include "hub_usb_descriptor.h"

static uint16_t read16(const uint8_t *data) {
    return (uint16_t)(data[0] | ((uint16_t)data[1] << 8));
}

bool hub_usb_inspect_interface(const uint8_t *data, size_t length, hub_usb_interface_t *result) {
    if (!data || !result) return false;
    memset(result, 0, sizeof(*result));
    if (length < 9 || data[0] < 9 || data[0] > length || data[1] != 4) return false;
    hub_usb_interface_t info = {
        .number = data[2], .alternate = data[3],
        .class_code = data[5], .subclass = data[6], .protocol = data[7]
    };
    unsigned endpoints = 0;
    unsigned event_inputs = 0, acl_inputs = 0, acl_outputs = 0;
    uint32_t seen = 0;
    for (size_t offset = data[0]; offset < length; ) {
        if (length - offset < 2) return false;
        const uint8_t *desc = data + offset;
        if (desc[0] < 2 || desc[0] > length - offset) return false;
        if (desc[1] == 4 || desc[1] == 11) break;
        if (desc[1] == 5) {
            if (desc[0] < 7) return false;
            uint8_t endpoint = desc[2], type = desc[3] & 3u;
            uint16_t packet_size = read16(desc + 4) & 0x7ffu;
            if ((endpoint & 0x70u) || !(endpoint & 0x0fu) || !packet_size) return false;
            unsigned index = (endpoint & 0x0fu) + ((endpoint & 0x80u) ? 16u : 0u);
            if (seen & (1u << index)) return false;
            seen |= 1u << index;
            ++endpoints;
            if (type == 3 && (endpoint & 0x80u)) {
                ++event_inputs;
                info.event_in = endpoint;
                info.event_size = packet_size;
            } else if (type == 2 && (endpoint & 0x80u)) {
                ++acl_inputs;
                info.acl_in = endpoint;
                info.acl_in_size = packet_size;
            } else if (type == 2) {
                ++acl_outputs;
                info.acl_out = endpoint;
                info.acl_out_size = packet_size;
            }
        }
        offset += desc[0];
    }
    if (endpoints != data[4]) return false;
    info.bluetooth_hci = info.alternate == 0 && info.class_code == 0xe0
        && info.subclass == 1 && info.protocol == 1 && endpoints == 3
        && event_inputs == 1 && acl_inputs == 1 && acl_outputs == 1
        && info.event_size <= 64 && info.acl_in_size <= 64 && info.acl_out_size <= 64;
    *result = info;
    return true;
}
