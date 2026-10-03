#include <assert.h>
#include <stdio.h>
#include "input_policy.h"

int main(void) {
    for (unsigned bits = 0; bits < 32; ++bits) {
        bool selected = (bits & 1) != 0;
        bool paused = (bits & 2) != 0;
        bool bonded = (bits & 4) != 0;
        bool pairing = (bits & 8) != 0;
        bool allow_open = (bits & 16) != 0;
        bool allowed = input_classic_admitted(selected, paused, bonded, pairing, allow_open);
        if (!selected || paused) assert(!allowed);
        else assert(allowed == (bonded || pairing || allow_open));
    }
    assert(input_classic_admitted(true, false, true, false, false));
    assert(!input_classic_admitted(true, false, false, false, false));
    assert(!input_classic_admitted(false, false, true, true, true));
    assert(!input_classic_admitted(true, true, true, true, true));
    for (unsigned bits = 0; bits < 64; ++bits) {
        bool selected = (bits & 1) != 0, paused = (bits & 2) != 0;
        bool pending = (bits & 4) != 0, incoming = (bits & 8) != 0;
        bool device = (bits & 16) != 0, acl = (bits & 32) != 0;
        assert(input_classic_acl_pending(selected, paused, pending, incoming, device, acl) ==
               (selected && !paused && pending && incoming && !device && acl));
    }

    assert(!input_pairing_window_open(true, false, 10, 0));
    assert(!input_pairing_window_open(false, false, 10, 20));
    assert(!input_pairing_window_open(true, true, 10, 20));
    assert(input_pairing_window_open(true, false, 10, 20));
    assert(!input_pairing_window_open(true, false, 20, 20));
    assert(!input_pairing_window_open(true, false, 21, 20));
    assert(input_pairing_window_open(true, false, 0xfffffff0u, 0x20u));
    assert(input_pairing_window_open(true, false, 0x10u, 0x20u));
    assert(!input_pairing_window_open(true, false, 0x20u, 0x20u));
    assert(!input_pairing_window_open(true, false, 0x10u, 0xfffffff0u));
    puts("ESP32 Classic bonded reconnect, unknown/paused admission and pairing-window expiry tests passed");
    return 0;
}
