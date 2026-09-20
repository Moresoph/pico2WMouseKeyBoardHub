#include "hub_classic_decode.h"
#include <stddef.h>
#include <string.h>

static uint16_t read16(const uint8_t *data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static void address_from_wire(uint8_t address[6], const uint8_t *wire) {
    for (unsigned i = 0; i < 6; ++i) address[i] = wire[5u - i];
}

bool hub_classic_command_decode(const uint8_t *packet, uint16_t length, hub_classic_metadata_t *out) {
    if (!packet || !out || length < 3 || (unsigned)packet[2] + 3u != length) return false;
    uint16_t opcode = read16(packet);
    if ((opcode == 0x0411 && length == 5)
        || ((opcode == 0x0413 || opcode == 0x0406) && length == 6)) {
        memset(out, 0, sizeof(*out));
        out->opcode = opcode;
        out->handle = read16(packet + 3);
        out->event = opcode == 0x0411 ? HUB_CLASSIC_AUTH_REQUEST
            : opcode == 0x0413 ? HUB_CLASSIC_ENCRYPTION_REQUEST : HUB_CLASSIC_DISCONNECT_REQUEST;
        if (length == 6) out->value = packet[5];
        return true;
    }
    if (!((opcode == 0x040d && length == 26) || (opcode == 0x040e && length == 9))) return false;
    memset(out, 0, sizeof(*out));
    out->opcode = opcode;
    out->event = opcode == 0x040d ? HUB_CLASSIC_PIN_REPLY : HUB_CLASSIC_PIN_NEGATIVE;
    address_from_wire(out->address, packet + 3);
    if (opcode == 0x040d) out->value = packet[9]; /* Length only; never copy the PIN. */
    return true;
}

bool hub_classic_l2cap_next(const uint8_t *packet, uint16_t length, unsigned *offset,
                           hub_classic_metadata_t *out) {
    if (!packet || !offset || !out || length < 12 || ((read16(packet) >> 12) & 3u) == 1
        || read16(packet + 6) != 1 || (uint32_t)read16(packet + 2) + 4u != length
        || (uint32_t)read16(packet + 4) + 8u != length) return false;
    unsigned position = *offset ? *offset : 8u;
    if (position < 8u || position > length) return false;
    while (position + 4u <= length) {
        uint8_t code = packet[position];
        unsigned size = read16(packet + position + 2u);
        if (size > length - position - 4u) return false;
        *offset = position + 4u + size;
        if ((code == 2 && size == 4) || (code == 3 && size == 8)) {
            memset(out, 0, sizeof(*out));
            out->handle = read16(packet) & 0x0fffu;
            const uint8_t *fields = packet + position + 4u;
            if (code == 2) {
                out->event = HUB_CLASSIC_L2CAP_REQUEST;
                out->psm = read16(fields);
                out->source_cid = read16(fields + 2);
            } else {
                out->event = HUB_CLASSIC_L2CAP_RESPONSE;
                out->destination_cid = read16(fields);
                out->source_cid = read16(fields + 2);
                out->result = read16(fields + 4);
                out->l2cap_status = read16(fields + 6);
            }
            return true;
        }
        position = *offset;
    }
    return false;
}

bool hub_classic_event_decode(const uint8_t *packet, uint16_t length, hub_classic_metadata_t *out) {
    if (!packet || !out || length < 2 || (unsigned)packet[1] + 2u != length) return false;
    memset(out, 0, sizeof(*out));
    switch (packet[0]) {
        case 0x16:
        case 0x17:
            if (length != 8) return false;
            out->event = packet[0] == 0x16 ? HUB_CLASSIC_PIN_REQUEST : HUB_CLASSIC_KEY_REQUEST;
            address_from_wire(out->address, packet + 2);
            return true;
        case 0x18:
            if (length != 25) return false;
            out->event = HUB_CLASSIC_KEY_CREATED;
            address_from_wire(out->address, packet + 2);
            out->value = packet[24]; /* Key type only; never copy the link key. */
            return true;
        case 0x06:
            if (length != 5) return false;
            out->event = HUB_CLASSIC_AUTH_COMPLETE;
            out->status = packet[2];
            out->handle = read16(packet + 3);
            return true;
        case 0x08:
        case 0x59:
            if (length < 6) return false;
            out->event = HUB_CLASSIC_ENCRYPTION;
            out->status = packet[2];
            out->handle = read16(packet + 3);
            out->value = packet[5];
            return true;
        case 0x05:
            if (length != 6) return false;
            out->event = HUB_CLASSIC_DISCONNECTED;
            out->status = packet[2];
            out->handle = read16(packet + 3);
            out->value = packet[5];
            return true;
        case 0x0e:
            if (length < 6) return false;
            out->opcode = read16(packet + 3);
            if (out->opcode != 0x040d && out->opcode != 0x040e) return false;
            out->event = HUB_CLASSIC_PIN_ACK;
            out->status = packet[5];
            if (length >= 12) address_from_wire(out->address, packet + 6);
            return true;
        case 0x07:
            if (length < 9) return false;
            out->event = HUB_CLASSIC_NAME;
            out->status = packet[2];
            address_from_wire(out->address, packet + 3);
            for (unsigned i = 0; i < 100 && i + 9u < length && packet[i + 9u]; ++i) {
                uint8_t c = packet[i + 9u];
                out->name[i] = c >= 32 && c < 127 ? (char)c : '.';
            }
            return true;
        default:
            return false;
    }
}
