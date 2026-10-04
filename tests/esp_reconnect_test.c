#include <assert.h>
#include <stdio.h>
#include "reconnect_policy.h"

static bool other_transport_busy(const input_reconnect_view_t slots[2], unsigned index) {
    unsigned other = 1u - index;
    return slots[other].classic == slots[index].classic && input_reconnect_busy(&slots[other]);
}

int main(void) {
    input_reconnect_view_t peers[2] = {
        {.selected = true, .classic = true},
        {.selected = true, .pending = true, .opening = true}
    };
    /* A blocked BLE open never owns the Classic scheduler or delays its deadline. */
    assert(input_reconnect_plan(&peers[0], 100, other_transport_busy(peers, 0)) == INPUT_OPEN);
    peers[0].pending = peers[0].acl = true;
    peers[0].started_at = 100;
    assert(input_reconnect_plan(&peers[0], 1099, other_transport_busy(peers, 0)) == INPUT_WAIT);
    assert(input_reconnect_plan(&peers[0], 1100, other_transport_busy(peers, 0)) == INPUT_ATTACH);
    peers[0].profile_requested = true;
    assert(input_reconnect_plan(&peers[0], 5101, false) == INPUT_WAIT);
    assert(input_reconnect_plan(&peers[0], 40100, false) == INPUT_RECOVER);
    peers[0].recovering = true;
    assert(input_reconnect_plan(&peers[0], 50000, false) == INPUT_WAIT);
    /* Cleanup must finish before another open; no permanent pause on transport timeout. */
    peers[0].pending = peers[0].recovering = peers[0].acl = peers[0].profile_requested = false;
    peers[0].retry_at = 51000;
    assert(input_reconnect_plan(&peers[0], 50999, false) == INPUT_WAIT);
    assert(input_reconnect_plan(&peers[0], 51000, false) == INPUT_OPEN);
    peers[0].device = true;
    assert(input_reconnect_plan(&peers[0], 51000, false) == INPUT_WAIT);
    peers[0].device = false;
    peers[0].opening = true;
    assert(input_reconnect_plan(&peers[0], 52000, false) == INPUT_WAIT);
    peers[0].pending = true;
    peers[0].started_at = 51000;
    assert(input_reconnect_plan(&peers[0], 91000, false) == INPUT_RECOVER);
    /* A BLE worker still running is not duplicated even after its deadline. */
    peers[1].recovering = true;
    assert(input_reconnect_plan(&peers[1], 120000, false) == INPUT_WAIT);
    assert(!other_transport_busy(peers, 0));
    /* Same-transport opens remain serialized. */
    peers[1].classic = true;
    assert(other_transport_busy(peers, 0));
    peers[0] = (input_reconnect_view_t){.selected = true, .classic = true};
    assert(input_reconnect_plan(&peers[0], 0, true) == INPUT_WAIT);
    peers[1].opening = peers[1].recovering = false;
    peers[1].acl = true;
    assert(!input_reconnect_busy(&peers[1]));
    peers[1].profile_requested = true;
    assert(input_reconnect_busy(&peers[1]));
    /* Security/manual pauses are never automatically overridden. */
    peers[0].paused = true;
    assert(input_reconnect_plan(&peers[0], 90000, false) == INPUT_WAIT);
    peers[0].paused = false;
    peers[0].pending = peers[0].mapped = true;
    peers[0].forwarding = false;
    assert(input_reconnect_plan(&peers[0], 40000, false) == INPUT_RECOVER);
    peers[0].forwarding = true;
    assert(input_reconnect_plan(&peers[0], 90000, false) == INPUT_WAIT);
    peers[0].forwarding = peers[0].mapped = false;
    peers[0].pairing = true;
    assert(input_reconnect_plan(&peers[0], 40000, false) == INPUT_WAIT);
    peers[0].pairing = false;
    assert(input_reconnect_plan(&peers[0], 40000, false) == INPUT_RECOVER);
    /* Exact deadlines, 32-bit timer wrap and capped retry backoff. */
    peers[0].started_at = 0xfffffff0u;
    assert(input_reconnect_plan(&peers[0], 0x9c2fu, false) == INPUT_WAIT);
    assert(input_reconnect_plan(&peers[0], 0x9c30u, false) == INPUT_RECOVER);
    peers[0].pending = false;
    peers[0].retry_at = 0x20;
    assert(input_reconnect_plan(&peers[0], 0xfffffff0u, false) == INPUT_WAIT);
    assert(input_reconnect_plan(&peers[0], 0x20, false) == INPUT_OPEN);
    const unsigned expected[] = {1000,1000,2000,4000,8000,15000,15000};
    for (unsigned i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i)
        assert(input_reconnect_backoff(i) == expected[i]);
    assert(input_reconnect_backoff(255) == 15000);
    puts("ESP32 production reconnect policy: BLE isolation, ACL/HID grace, timeout recovery, no duplicate workers, safety pauses and wrap/backoff passed");
    return 0;
}
