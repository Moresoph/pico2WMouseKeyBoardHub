#include "hub_smp_decode.h"
#include <stddef.h>
#include <string.h>

static uint16_t read16(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

bool hub_smp_decode(const uint8_t *packet, uint16_t length, hub_smp_metadata_t *metadata) {
    if (packet == NULL || metadata == NULL || length < 9) return false;
    uint16_t flags = read16(packet);
    if (((flags >> 12) & 3u) == 1u) return false;
    uint16_t acl_length = read16(packet + 2);
    uint16_t smp_length = read16(packet + 4);
    if (read16(packet + 6) != 6 || acl_length < 4
        || (uint32_t)acl_length + 4u != length || (uint32_t)smp_length + 4u != acl_length) {
        return false;
    }
    const uint8_t *smp = packet + 8;
    if (!((smp[0] == 1 || smp[0] == 2) && smp_length == 7)
        && !((smp[0] == 5 || smp[0] == 11) && smp_length == 2)) return false;
    memset(metadata, 0, sizeof(*metadata));
    metadata->handle = flags & 0x0fffu;
    metadata->code = smp[0];
    if (smp[0] == 1 || smp[0] == 2) {
        metadata->io = smp[1];
        metadata->oob = smp[2];
        metadata->auth = smp[3];
        metadata->key_size = smp[4];
        metadata->initiator_keys = smp[5];
        metadata->responder_keys = smp[6];
    } else if (smp[0] == 5) {
        metadata->reason = smp[1];
    } else {
        metadata->auth = smp[1];
    }
    return true;
}
