#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hub_hid_report.h"
#include "hub_remote_hid.h"

static const uint8_t boot_keyboard[] = {
    5,1,9,6,0xa1,1,5,7,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,
    0x75,1,0x95,8,0x81,2,0x75,8,0x95,1,0x81,3,
    0x19,0,0x29,0xff,0x15,0,0x26,0xff,0,0x75,8,0x95,6,0x81,0,0xc0
};
int main(void) {
    hub_remote_hid_t hid = {0};
    hub_link_input_t state = {0};
    assert(hub_remote_hid_map(&hid, 0, hub_hid_report_descriptor, sizeof(hub_hid_report_descriptor)));
    assert(hid.roles == 3 && hid.count == 2);
    uint8_t keyboard[8] = {5,0,4,7};
    assert(hub_remote_hid_report(&hid, 0, 1, keyboard, sizeof(keyboard), &state));
    assert(state.keyboard[0] == 5 && state.keyboard[2] == 4 && state.keyboard[3] == 7);
    uint8_t mouse[6] = {3,0xfe,0xff,0,1,0xff};
    assert(hub_remote_hid_report(&hid, 0, 2, mouse, sizeof(mouse), &state));
    assert(state.dx == -2 && state.dy == 256 && state.wheel == -1 && state.buttons == 3);
    assert(state.keyboard[2] == 4); /* Separate report IDs retain each other's state. */
    assert(!hub_remote_hid_report(&hid, 0, 1, keyboard, 7, &state));
    assert(!hub_remote_hid_report(&hid, 0, 9, keyboard, 8, &state));
    assert(!hub_remote_hid_report(&hid, 3, 1, keyboard, 8, &state));
    memset(keyboard, 0, 8);
    assert(hub_remote_hid_report(&hid, 0, 1, keyboard, 8, &state));
    assert(!state.keyboard[2] && state.buttons == 3 && state.dx == 0);
    memset(&hid, 0, sizeof(hid));
    assert(hub_remote_hid_map(&hid, 0, boot_keyboard, sizeof(boot_keyboard)));
    keyboard[2] = 0x4f; keyboard[0] = 5;
    assert(hub_remote_hid_report(&hid, 0, 0, keyboard, 8, &state));
    assert(state.keyboard[0] == 5 && state.keyboard[2] == 0x4f);
    keyboard[2] = 1;
    assert(hub_remote_hid_report(&hid, 0, 0, keyboard, 8, &state));
    for (unsigned i = 2; i < 8; ++i) assert(state.keyboard[i] == 1);
    assert(!hub_remote_hid_map(&hid, 0, boot_keyboard, sizeof(boot_keyboard)));
    for (unsigned length = 0; length < sizeof(boot_keyboard); ++length) {
        memset(&hid, 0, sizeof(hid));
        bool valid = hub_remote_hid_map(&hid, 0, boot_keyboard, (uint16_t)length);
        assert(!valid || !hid.roles); /* A globals-only prefix has no routable input. */
    }
    uint8_t bad[sizeof(hub_hid_report_descriptor)];
    memcpy(bad, hub_hid_report_descriptor, sizeof(bad));
    for (unsigned i = 0; i + 1 < sizeof(bad); ++i)
        if (bad[i] == 0x81 && bad[i + 1] == 6) bad[i + 1] = 2;
    memset(&hid, 0, sizeof(hid));
    assert(!hub_remote_hid_map(&hid, 0, bad, sizeof(bad))); /* Absolute axes cannot become deltas. */
    puts("remote HID keyboard, mouse, report ID, malformed and absolute-axis tests passed");
    return 0;
}
