#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hub_classic_decode.h"

int main(void) {
    hub_classic_metadata_t metadata;
    uint8_t reply[26] = {0x0d, 0x04, 23, 0x98, 0xb4, 0x3b, 0, 0x18, 0, 6};
    memset(reply + 10, 0xa5, 16);
    assert(hub_classic_command_decode(reply, sizeof(reply), &metadata));
    assert(metadata.event == HUB_CLASSIC_PIN_REPLY && metadata.value == 6);
    assert(metadata.address[0] == 0 && metadata.address[1] == 0x18);
    for (unsigned i = 0; i < sizeof(metadata); ++i) {
        assert(((uint8_t *)&metadata)[i] != 0xa5);
    }
    uint8_t ack[] = {0x0e, 4, 1, 0x0d, 0x04, 0};
    assert(hub_classic_event_decode(ack, sizeof(ack), &metadata));
    assert(metadata.event == HUB_CLASSIC_PIN_ACK && metadata.status == 0);
    uint8_t key[25] = {0x18, 23, 0x98, 0xb4, 0x3b, 0, 0x18, 0};
    memset(key + 8, 0xa5, 16);
    key[24] = 4;
    assert(hub_classic_event_decode(key, sizeof(key), &metadata));
    assert(metadata.event == HUB_CLASSIC_KEY_CREATED && metadata.value == 4);
    for (unsigned i = 0; i < sizeof(metadata); ++i) {
        assert(((uint8_t *)&metadata)[i] != 0xa5);
    }
    uint8_t auth[] = {6, 3, 5, 0x0b, 0};
    assert(hub_classic_event_decode(auth, sizeof(auth), &metadata));
    assert(metadata.event == HUB_CLASSIC_AUTH_COMPLETE && metadata.status == 5);
    assert(metadata.handle == 0x0b);
    uint8_t name[] = {7, 13, 0, 0x98, 0xb4, 0x3b, 0, 0x18, 0, 'F', 'I', 0x1b, 'C', 'O', 0};
    assert(hub_classic_event_decode(name, sizeof(name), &metadata));
    assert(strcmp(metadata.name, "FI.CO") == 0);
    for (unsigned i = 0; i < sizeof(reply); ++i) {
        assert(!hub_classic_command_decode(reply, (uint16_t)i, &metadata));
    }
    for (unsigned i = 0; i < sizeof(key); ++i) {
        assert(!hub_classic_event_decode(key, (uint16_t)i, &metadata));
    }
    uint8_t unsupported[] = {0xff, 0};
    assert(!hub_classic_event_decode(unsupported, sizeof(unsupported), &metadata));
    uint8_t authenticate[] = {0x11, 4, 2, 0x0b, 0};
    assert(hub_classic_command_decode(authenticate, sizeof(authenticate), &metadata));
    assert(metadata.event == HUB_CLASSIC_AUTH_REQUEST && metadata.handle == 11);
    uint8_t signal[] = {
        0x0b, 0x20, 24, 0, 20, 0, 1, 0,
        2, 1, 4, 0, 0x11, 0, 0x40, 0,
        3, 2, 8, 0, 0x41, 0, 0x40, 0, 3, 0, 0, 0
    };
    unsigned offset = 0;
    assert(hub_classic_l2cap_next(signal, sizeof(signal), &offset, &metadata));
    assert(metadata.event == HUB_CLASSIC_L2CAP_REQUEST && metadata.psm == 0x11);
    assert(hub_classic_l2cap_next(signal, sizeof(signal), &offset, &metadata));
    assert(metadata.event == HUB_CLASSIC_L2CAP_RESPONSE && metadata.result == 3);
    assert(!hub_classic_l2cap_next(signal, sizeof(signal), &offset, &metadata));
    offset = 0;
    signal[10] = 255;
    assert(!hub_classic_l2cap_next(signal, sizeof(signal), &offset, &metadata));
    puts("All Classic metadata privacy/bounds tests passed.");
    return 0;
}
