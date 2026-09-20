#ifndef HUB_INPUT_POLICY_H
#define HUB_INPUT_POLICY_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define HUB_INPUT_TARGET_BYTES 16u
#define HUB_INPUT_ROLE_KEYBOARD 1u
#define HUB_INPUT_ROLE_MOUSE 2u
#define HUB_INPUT_ROLE_BOTH 3u

typedef struct {
    bool classic;
    bool insecure;
    bool forwarding;
    uint8_t type;
    uint8_t roles;
    uint8_t address[6];
} hub_input_target_t;

static inline bool hub_input_stable_address(bool classic, uint8_t type,
                                             const uint8_t address[6]) {
    const uint8_t zero[6] = {0};
    const uint8_t ones[6] = {255, 255, 255, 255, 255, 255};
    if (type > 1 || (classic && type) || !memcmp(address, zero, 6) ||
        !memcmp(address, ones, 6)) return false;
    /* Addresses are in display order, not HCI wire order. */
    return classic || !type || (address[0] & 0xc0u) == 0xc0u;
}

static inline uint16_t hub_input_target_crc(const uint8_t *data, unsigned length) {
    uint16_t crc = 0xffffu;
    for (unsigned i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000u) ? 0x1021u : 0u));
        }
    }
    return crc;
}

static inline void hub_input_target_encode(const hub_input_target_t *target,
                                            uint8_t data[HUB_INPUT_TARGET_BYTES]) {
    memset(data, 0, HUB_INPUT_TARGET_BYTES);
    data[0] = 'H';
    data[1] = 'I';
    data[2] = target->insecure ? 2 : 1;
    data[3] = target->classic ? 1 : 2;
    data[4] = target->type;
    data[5] = target->roles;
    memcpy(data + 6, target->address, 6);
    if (target->insecure) data[12] = 1u | (target->forwarding ? 2u : 0u);
    uint16_t crc = hub_input_target_crc(data, 14);
    data[14] = (uint8_t)crc;
    data[15] = (uint8_t)(crc >> 8);
}

static inline bool hub_input_target_decode(const uint8_t *data, unsigned length,
                                            hub_input_target_t *target) {
    if (length != HUB_INPUT_TARGET_BYTES || data[0] != 'H' || data[1] != 'I' ||
        (data[2] != 1 && data[2] != 2) || (data[3] != 1 && data[3] != 2) || data[13] ||
        (data[2] == 1 && data[12]) ||
        (data[2] == 2 && (data[3] != 1 || (data[12] != 1 && data[12] != 3))) ||
        !data[5] || (data[5] & ~HUB_INPUT_ROLE_BOTH) ||
        hub_input_target_crc(data, 14) != ((uint16_t)data[14] | ((uint16_t)data[15] << 8)) ||
        !hub_input_stable_address(data[3] == 1, data[4], data + 6)) return false;
    target->classic = data[3] == 1;
    target->insecure = data[2] == 2;
    target->forwarding = target->insecure && (data[12] & 2u);
    target->type = data[4];
    target->roles = data[5];
    memcpy(target->address, data + 6, 6);
    return true;
}

static inline uint8_t hub_input_assign_roles(uint8_t advertised, uint8_t reserved) {
    /* A composite first device must not prevent enrolling the user's second device. */
    if (reserved == HUB_INPUT_ROLE_BOTH) return advertised & HUB_INPUT_ROLE_BOTH;
    return advertised & (uint8_t)~reserved & HUB_INPUT_ROLE_BOTH;
}

static inline bool hub_input_ble_discoverable(const uint8_t *data, unsigned length) {
    bool discoverable = false;
    for (unsigned pos = 0; pos < length;) {
        unsigned field = data[pos];
        if (!field) break;
        if (pos + field + 1u > length) return false;
        if (data[pos + 1u] == 1 && field == 2) {
            discoverable = (data[pos + 2u] & 3u) != 0;
        }
        pos += field + 1u;
    }
    return discoverable;
}

static inline bool hub_input_enrollment_permitted(bool active, uint32_t now,
                                                  uint32_t deadline, bool pc_pairing) {
    return active && !pc_pairing && (int32_t)(now - deadline) < 0;
}

static inline uint8_t hub_input_classic_roles(uint32_t cod) {
    if ((cod & 0x1f00u) != 0x0500u) return 0;
    return ((cod & 0x40u) ? HUB_INPUT_ROLE_KEYBOARD : 0) |
           ((cod & 0x80u) ? HUB_INPUT_ROLE_MOUSE : 0);
}

static inline bool hub_input_ble_hid_advertisement(const uint8_t *data, unsigned length) {
    bool hid = false;
    for (unsigned pos = 0; pos < length;) {
        unsigned field = data[pos];
        if (!field) break;
        if (pos + field + 1u > length) return false;
        uint8_t type = data[pos + 1u];
        if (type == 2 || type == 3) {
            if ((field - 1u) & 1u) return false;
            for (unsigned i = pos + 2; i + 1 < pos + field + 1u; i += 2) {
                if (data[i] == 0x12 && data[i + 1] == 0x18) hid = true;
            }
        } else if (type == 0x19 && field == 3) {
            uint16_t appearance = (uint16_t)data[pos + 2] | ((uint16_t)data[pos + 3] << 8);
            if (appearance == 0x03c1u || appearance == 0x03c2u) hid = true;
        } else if (type == 0x16 && field >= 3) {
            if (data[pos + 2] == 0x12 && data[pos + 3] == 0x18) hid = true;
        }
        pos += field + 1u;
    }
    return hid;
}

#endif
