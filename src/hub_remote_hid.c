#include "hub_remote_hid.h"
#include "btstack_hid_parser.h"
#include <string.h>

static hub_remote_report_t *find_report(hub_remote_hid_t *h, unsigned map, unsigned id, bool create) {
    for (unsigned i = 0; i < h->count; ++i)
        if (h->reports[i].map == map && h->reports[i].id == id) return &h->reports[i];
    if (!create || h->count == HUB_REMOTE_REPORTS) return NULL;
    hub_remote_report_t *r = &h->reports[h->count++];
    memset(r, 0, sizeof(*r));
    r->map = (uint8_t)map;
    r->id = (uint8_t)id;
    return r;
}

bool hub_remote_hid_map(hub_remote_hid_t *h, unsigned map, const uint8_t *data, uint16_t length) {
    if (map >= HUB_REMOTE_MAPS || !data || !length || length > 4096 || h->maps[map].data) return false;
    unsigned id = 0, size = 0, count = 0, depth = 0, app = 0;
    unsigned apps[16];
    uint32_t page = 0, usage = 0;
    bool numbered = false, seen[256] = {false};
    btstack_hid_descriptor_iterator_t it;
    btstack_hid_descriptor_iterator_init(&it, data, length);
    while (btstack_hid_descriptor_iterator_has_more(&it)) {
        const hid_descriptor_item_t *item = btstack_hid_descriptor_iterator_get_item(&it);
        uint32_t value = (uint32_t)item->item_value;
        if (item->item_type == Reserved || item->item_size > 5) return false;
        if (item->item_type == Global) {
            switch (item->item_tag) {
                case UsagePage: if (value > 65535) return false; page = value; break;
                case ReportSize: if (value > 32) return false; size = value; break;
                case ReportCount: if (value > 255) return false; count = value; break;
                case ReportID:
                    if (!value || value > 255 || seen[0] || (id != value && seen[value])) return false;
                    id = value;
                    seen[id] = true;
                    numbered = true;
                    break;
                case Push: case Pop: return false; /* Same bounded parser policy as Pico inputs. */
                default: if (item->item_tag > Pop) return false; break;
            }
        } else if (item->item_type == Local) {
            if (item->item_tag == Usage) usage = item->data_size > 2 ? value : (page << 16) | value;
            if (item->item_tag == Delimiter) return false;
        } else if (item->item_type == Main) {
            if (item->item_tag == Coll) {
                if (depth == 16) return false;
                apps[depth++] = app;
                if (value == 1) app = usage == 0x10006 || usage == 0x10007
                    ? HUB_LINK_KEYBOARD : usage == 0x10002 ? HUB_LINK_MOUSE : 0;
            } else if (item->item_tag == EndColl) {
                if (!depth) return false;
                app = apps[--depth];
            } else if (item->item_tag == Input) {
                if (!size || !count || !depth) return false;
                seen[id] = true;
                hub_remote_report_t *r = find_report(h, map, id, true);
                if (!r || size * count > 1024u - r->bits) return false;
                r->bits += (uint16_t)(size * count);
                if (!(value & 1)) r->roles |= (uint8_t)app;
            } else if (item->item_tag != Output && item->item_tag != Feature) return false;
            usage = 0;
        }
    }
    if (depth || !btstack_hid_descriptor_iterator_valid(&it)) return false;
    uint8_t actual[HUB_REMOTE_REPORTS] = {0};
    btstack_hid_usage_iterator_t usages;
    btstack_hid_usage_iterator_init(&usages, data, length, HID_REPORT_TYPE_INPUT);
    while (btstack_hid_usage_iterator_has_more(&usages)) {
        btstack_hid_usage_item_t item;
        btstack_hid_usage_iterator_get_item(&usages, &item);
        unsigned rid = item.report_id == HID_REPORT_ID_UNDEFINED ? 0 : item.report_id;
        hub_remote_report_t *r = find_report(h, map, rid, false);
        if (!r || !item.size || item.size > 32 || item.bit_pos + item.size > r->bits ||
            ((item.bit_pos & 7u) + item.size + 7u) / 8u > 4u) return false;
        unsigned index = (unsigned)(r - h->reports);
        if (item.usage_page == 7) {
            if (!(r->roles & HUB_LINK_KEYBOARD)) return false;
            actual[index] |= HUB_LINK_KEYBOARD;
        } else if (item.usage_page == 9 && (r->roles & HUB_LINK_MOUSE)) {
            if (!(item.descriptor_item.item_value & 2)) return false;
            if (item.usage >= 1 && item.usage <= 5) actual[index] |= HUB_LINK_MOUSE;
        } else if (item.usage_page == 1 &&
                   (item.usage == 0x30 || item.usage == 0x31 || item.usage == 0x38)) {
            if (!(r->roles & HUB_LINK_MOUSE) || (item.descriptor_item.item_value & 6) != 6 ||
                item.global_logical_minimum >= 0) return false;
            actual[index] |= HUB_LINK_MOUSE;
        }
    }
    for (unsigned i = 0; i < h->count; ++i) {
        if (h->reports[i].map == map) h->reports[i].roles = actual[i];
        h->roles |= h->reports[i].roles;
    }
    h->maps[map] = (hub_remote_map_t){data, length, numbered};
    return true;
}

bool hub_remote_hid_report(hub_remote_hid_t *h, unsigned map, unsigned id,
                           const uint8_t *data, size_t length, hub_link_input_t *state) {
    if (map >= HUB_REMOTE_MAPS || id > 255 || !data || !h->maps[map].data) return false;
    hub_remote_report_t *r = find_report(h, map, id, false);
    if (!r || length != (r->bits + 7u) / 8u || length > 128) return false;
    const hub_remote_map_t *m = &h->maps[map];
    uint8_t wire[129], keys[32] = {0}, buttons = r->buttons;
    unsigned offset = m->numbered ? 1u : 0u;
    if (offset) wire[0] = (uint8_t)id;
    memcpy(wire + offset, data, length);
    int64_t axes[3] = {0};
    btstack_hid_parser_t parser;
    btstack_hid_parser_init(&parser, m->data, m->length, HID_REPORT_TYPE_INPUT, wire, length + offset);
    while (btstack_hid_parser_has_more(&parser)) {
        uint16_t page, usage;
        int32_t value;
        btstack_hid_parser_get_field(&parser, &page, &usage, &value);
        if (page == 7 && (r->roles & HUB_LINK_KEYBOARD)) {
            if (value && usage && usage < 256) keys[usage / 8u] |= (uint8_t)(1u << (usage & 7u));
        } else if (page == 9 && (r->roles & HUB_LINK_MOUSE) && usage >= 1 && usage <= 5) {
            uint8_t mask = (uint8_t)(1u << (usage - 1u));
            buttons = value ? buttons | mask : buttons & (uint8_t)~mask;
        } else if (page == 1 && (r->roles & HUB_LINK_MOUSE)) {
            if (usage == 0x30) axes[0] += value;
            if (usage == 0x31) axes[1] += value;
            if (usage == 0x38) axes[2] += value;
        }
    }
    if (axes[0] < -32768 || axes[0] > 32767 || axes[1] < -32768 || axes[1] > 32767 ||
        axes[2] < -127 || axes[2] > 127) return false;
    memcpy(r->keys, keys, sizeof(keys));
    r->buttons = buttons;
    memset(keys, 0, sizeof(keys));
    state->buttons = 0;
    for (unsigned i = 0; i < h->count; ++i) {
        for (unsigned k = 0; k < 32; ++k) keys[k] |= h->reports[i].keys[k];
        state->buttons |= h->reports[i].buttons;
    }
    memset(state->keyboard, 0, sizeof(state->keyboard));
    state->keyboard[0] = keys[28];
    unsigned count = 0;
    for (unsigned key = 4; key < 256; ++key) {
        if (key >= 0xe0 && key <= 0xe7) continue;
        if (keys[key / 8u] & (1u << (key & 7u))) {
            if (count < 6) state->keyboard[2 + count] = (uint8_t)key;
            ++count;
        }
    }
    if (count > 6 || (keys[0] & 0x0e)) memset(state->keyboard + 2, 1, 6);
    state->roles = h->roles;
    state->dx = (int32_t)axes[0];
    state->dy = (int32_t)axes[1];
    state->wheel = (int32_t)axes[2];
    return true;
}
