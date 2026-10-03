#include "hub_link.h"
#include <string.h>

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int32_t signed32(const uint8_t *p) {
    uint32_t value = get32(p);
    return value <= INT32_MAX ? (int32_t)value : -1 - (int32_t)(UINT32_MAX - value);
}

static void put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8u * i));
}

static uint16_t crc16(const uint8_t *data, size_t length) {
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    return crc;
}

static bool frame_valid(const hub_link_frame_t *f) {
    if (!f->nonce) return false;
    if (f->type == HUB_LINK_REQUEST) return !f->length && !f->session;
    if (!f->session) return false;
    if (f->type == HUB_LINK_STATE) return f->length == HUB_LINK_PAYLOAD;
    if (f->type == HUB_LINK_OUTPUT) return f->length == HUB_LINK_OUTPUT_PAYLOAD;
    if (f->type == HUB_LINK_UI) return f->length == 1 && f->payload[0] <= 1;
    if (f->type == HUB_LINK_MANAGE) return f->length == 12;
    return f->type == HUB_LINK_MANAGE_REPLY && f->length == 5 && f->payload[4] <= 1;
}

size_t hub_link_encode(const hub_link_frame_t *f, uint8_t wire[HUB_LINK_WIRE_MAX]) {
    if (!f || !wire || !frame_valid(f)) return 0;
    uint8_t raw[HUB_LINK_RAW_MAX] = {1, f->type};
    put32(raw + 2, f->nonce);
    put32(raw + 6, f->session);
    raw[10] = (uint8_t)f->sequence;
    raw[11] = (uint8_t)(f->sequence >> 8);
    raw[12] = f->length;
    memcpy(raw + 13, f->payload, f->length);
    size_t length = 13u + f->length;
    uint16_t crc = crc16(raw, length);
    raw[length++] = (uint8_t)crc;
    raw[length++] = (uint8_t)(crc >> 8);
    size_t n = 0;
    wire[n++] = 0x7e;
    for (size_t i = 0; i < length; ++i) {
        if (raw[i] == 0x7e || raw[i] == 0x7d) {
            wire[n++] = 0x7d;
            wire[n++] = raw[i] ^ 0x20;
        } else wire[n++] = raw[i];
    }
    wire[n++] = 0x7e;
    return n;
}

bool hub_link_feed(hub_link_parser_t *p, uint8_t byte, hub_link_frame_t *f) {
    if (byte == 0x7e) {
        bool valid = false;
        if (p->started && (p->length || p->discard || p->escaped)) {
            unsigned n = p->length;
            if (!p->discard && !p->escaped && n >= 15 && p->bytes[0] == 1 &&
                p->bytes[12] <= HUB_LINK_FRAME_PAYLOAD && n == 15u + p->bytes[12] &&
                crc16(p->bytes, n - 2) ==
                    (uint16_t)(p->bytes[n - 2] | (uint16_t)p->bytes[n - 1] << 8)) {
                memset(f, 0, sizeof(*f));
                f->type = p->bytes[1];
                f->nonce = get32(p->bytes + 2);
                f->session = get32(p->bytes + 6);
                f->sequence = (uint16_t)(p->bytes[10] | (uint16_t)p->bytes[11] << 8);
                f->length = p->bytes[12];
                memcpy(f->payload, p->bytes + 13, f->length);
                valid = frame_valid(f);
            }
            if (!valid) ++p->errors;
        }
        p->started = true;
        p->length = 0;
        p->escaped = p->discard = false;
        return valid;
    }
    if (!p->started || p->discard) return false;
    if (!p->escaped && byte == 0x7d) {
        p->escaped = true;
        return false;
    }
    if (p->escaped) {
        if (byte != 0x5e && byte != 0x5d) {
            p->discard = true;
            return false;
        }
        byte ^= 0x20;
        p->escaped = false;
    }
    if (p->length == sizeof(p->bytes)) p->discard = true;
    else p->bytes[p->length++] = byte;
    return false;
}

void hub_link_pack(hub_link_frame_t *f, const hub_link_input_t inputs[HUB_LINK_SLOTS]) {
    f->type = HUB_LINK_STATE;
    f->length = HUB_LINK_PAYLOAD;
    for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i) {
        const hub_link_input_t *s = &inputs[i];
        uint8_t *p = f->payload + i * 23u;
        p[0] = s->epoch;
        p[1] = s->roles;
        memcpy(p + 2, s->keyboard, 8);
        p[10] = s->buttons;
        put32(p + 11, (uint32_t)s->dx);
        put32(p + 15, (uint32_t)s->dy);
        put32(p + 19, (uint32_t)s->wheel);
    }
}

bool hub_link_unpack(const hub_link_frame_t *f, hub_link_input_t inputs[HUB_LINK_SLOTS]) {
    if (f->type != HUB_LINK_STATE || f->length != HUB_LINK_PAYLOAD) return false;
    hub_link_input_t decoded[HUB_LINK_SLOTS];
    for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i) {
        const uint8_t *p = f->payload + i * 23u;
        hub_link_input_t *s = &decoded[i];
        s->epoch = p[0];
        s->roles = p[1];
        memcpy(s->keyboard, p + 2, 8);
        s->buttons = p[10];
        s->dx = signed32(p + 11);
        s->dy = signed32(p + 15);
        s->wheel = signed32(p + 19);
        if (s->roles > 3 || s->keyboard[1] || s->buttons > 31 ||
            s->dx < -32768 || s->dx > 32767 || s->dy < -32768 || s->dy > 32767 ||
            s->wheel < -127 || s->wheel > 127) return false;
        if (!(s->roles & HUB_LINK_KEYBOARD)) {
            for (unsigned k = 0; k < 8; ++k) if (s->keyboard[k]) return false;
        }
        if (!(s->roles & HUB_LINK_MOUSE) && (s->buttons || s->dx || s->dy || s->wheel))
            return false;
    }
    memcpy(inputs, decoded, sizeof(decoded));
    return true;
}

void hub_link_pack_output(hub_link_frame_t *f, const hub_link_output_t *s) {
    f->type = HUB_LINK_OUTPUT;
    f->length = HUB_LINK_OUTPUT_PAYLOAD;
    f->payload[0] = 1;
    f->payload[1] = s->selected;
    f->payload[2] = s->flags;
    memcpy(f->payload + 3, s->version, 3);
    for (unsigned i = 0; i < HUB_LINK_PC_SLOTS; ++i) {
        uint8_t *p = f->payload + 6 + i * 10;
        const hub_link_pc_t *pc = &s->computers[i];
        p[0] = pc->flags;
        p[1] = pc->address_type;
        memcpy(p + 2, pc->address, 6);
        p[8] = (uint8_t)pc->interval;
        p[9] = (uint8_t)(pc->interval >> 8);
    }
}

bool hub_link_unpack_output(const hub_link_frame_t *f, hub_link_output_t *s) {
    if (f->type != HUB_LINK_OUTPUT || f->length != HUB_LINK_OUTPUT_PAYLOAD ||
        f->payload[0] != 1 || !f->payload[1] || f->payload[1] > HUB_LINK_PC_SLOTS ||
        (f->payload[2] & ~31u)) return false;
    hub_link_output_t value = {.selected = f->payload[1], .flags = f->payload[2]};
    memcpy(value.version, f->payload + 3, 3);
    for (unsigned i = 0; i < HUB_LINK_PC_SLOTS; ++i) {
        const uint8_t *p = f->payload + 6 + i * 10;
        hub_link_pc_t *pc = &value.computers[i];
        pc->flags = p[0];
        pc->address_type = p[1];
        memcpy(pc->address, p + 2, 6);
        pc->interval = (uint16_t)(p[8] | (uint16_t)p[9] << 8);
        if ((pc->flags & ~31u) || pc->address_type > 1 ||
            (!(pc->flags & HUB_LINK_PC_RESERVED) && pc->flags) ||
            (!(pc->flags & HUB_LINK_PC_CONNECTED) &&
             ((pc->flags & (HUB_LINK_PC_ENCRYPTED | HUB_LINK_PC_READY | HUB_LINK_PC_SUSPENDED)) || pc->interval)) ||
            ((pc->flags & HUB_LINK_PC_CONNECTED) && (pc->interval < 6 || pc->interval > 3200)) ||
            ((pc->flags & HUB_LINK_PC_READY) &&
             (!(pc->flags & HUB_LINK_PC_ENCRYPTED) || (pc->flags & HUB_LINK_PC_SUSPENDED)))) return false;
    }
    *s = value;
    return true;
}

bool hub_link_receive_output(hub_link_output_cache_t *cache, const hub_link_frame_t *f,
                             uint32_t nonce, uint32_t session, uint32_t now) {
    if (!nonce || !session || f->nonce != nonce || f->session != session) return false;
    if (cache->valid && cache->nonce == nonce && cache->session == session) {
        uint16_t distance = (uint16_t)(f->sequence - cache->sequence);
        if (!distance || distance >= 0x8000u) return false;
    }
    hub_link_output_t value;
    if (!hub_link_unpack_output(f, &value)) return false;
    *cache = (hub_link_output_cache_t){.valid = true, .nonce = nonce, .session = session,
        .received_at = now, .sequence = f->sequence, .value = value};
    return true;
}

bool hub_link_output_fresh(const hub_link_output_cache_t *cache,
                           uint32_t nonce, uint32_t session, uint32_t now) {
    return cache->valid && nonce && session && cache->nonce == nonce && cache->session == session &&
        (uint32_t)(now - cache->received_at) < HUB_LINK_OUTPUT_TIMEOUT_MS;
}
