#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hub_smp_decode.h"

int main(void) {
    hub_smp_metadata_t metadata;
    uint8_t request[] = {0x41, 0x20, 11, 0, 7, 0, 6, 0, 1, 1, 0, 9, 16, 3, 3};
    assert(hub_smp_decode(request, sizeof(request), &metadata));
    assert(metadata.handle == 0x41 && metadata.code == 1 && metadata.io == 1);
    assert(metadata.auth == 9 && metadata.key_size == 16);
    assert(metadata.initiator_keys == 3 && metadata.responder_keys == 3);
    request[8] = 2;
    request[9] = 3;
    assert(hub_smp_decode(request, sizeof(request), &metadata));
    assert(metadata.code == 2 && metadata.io == 3);
    uint8_t failed[] = {0x41, 0, 6, 0, 2, 0, 6, 0, 5, 3};
    assert(hub_smp_decode(failed, sizeof(failed), &metadata) && metadata.reason == 3);
    failed[8] = 11;
    assert(hub_smp_decode(failed, sizeof(failed), &metadata) && metadata.auth == 3);
    failed[6] = 4;
    assert(!hub_smp_decode(failed, sizeof(failed), &metadata));
    uint8_t secret[25] = {0x41, 0x20, 21, 0, 17, 0, 6, 0};
    memset(secret + 9, 0xab, 16);
    for (unsigned code = 3; code <= 14; ++code) {
        secret[8] = (uint8_t)code;
        assert(!hub_smp_decode(secret, sizeof(secret), &metadata));
    }
    for (unsigned length = 0; length < sizeof(request); ++length) {
        assert(!hub_smp_decode(request, (uint16_t)length, &metadata));
    }
    request[1] = 0x10;
    assert(!hub_smp_decode(request, sizeof(request), &metadata));
    assert(!hub_smp_decode(NULL, 0, &metadata));
    puts("All SMP metadata privacy/bounds tests passed.");
    return 0;
}
