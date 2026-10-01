#include <string.h>
#include "btstack_hid_parser.h"
#include "hub_usb_mouse_decode.h"

bool hub_usb_mouse_layout(hub_usb_mouse_layout_t *layout, const uint8_t *descriptor, uint16_t length) {
    memset(layout, 0, sizeof(*layout));
    if (!descriptor || !length || length > 1024) return false;
    uint16_t bits[256] = {0};
    unsigned size = 0, count = 0, id = 0, total = 0, depth = 0;
    btstack_hid_descriptor_iterator_t iterator;
    btstack_hid_descriptor_iterator_init(&iterator, descriptor, length);
    while (btstack_hid_descriptor_iterator_has_more(&iterator)) {
        const hid_descriptor_item_t *item = btstack_hid_descriptor_iterator_get_item(&iterator);
        if (item->item_type == Reserved || item->item_size > 5) return false;
        uint32_t value = (uint32_t)item->item_value;
        if (item->item_type == Global) {
            switch (item->item_tag) {
                case Push: case Pop: return false;
                case ReportSize:
                    if (value > 32) return false;
                    size = value;
                    break;
                case ReportCount:
                    if (value > 255) return false;
                    count = value;
                    break;
                case ReportID:
                    if (!value || value > 255 || bits[value] || bits[0]) return false;
                    id = value;
                    layout->numbered = true;
                    break;
                default: break;
            }
        } else if (item->item_type == Main) {
            if (item->item_tag == Coll) {
                if (++depth > 16) return false;
            } else if (item->item_tag == EndColl) {
                if (!depth) return false;
                --depth;
            } else if (item->item_tag == Input) {
                unsigned added = size * count;
                if (!added || added > 512 || bits[id] + added > 512 || total + added > 4096)
                    return false;
                bits[id] += (uint16_t)added;
                total += added;
            }
        } else if (item->item_type == Local && item->item_tag == Delimiter) {
            return false;
        }
    }
    if (!btstack_hid_descriptor_iterator_valid(&iterator) || depth) return false;
    for (unsigned i = 0; i < 256; ++i) {
        unsigned bytes = (bits[i] + 7u) / 8u + (layout->numbered && bits[i] ? 1u : 0u);
        if (bytes > 64) return false;
        layout->sizes[i] = (uint8_t)bytes;
    }

    bool have_motion = false;
    btstack_hid_usage_iterator_t usages;
    btstack_hid_usage_iterator_init(&usages, descriptor, length, HID_REPORT_TYPE_INPUT);
    while (btstack_hid_usage_iterator_has_more(&usages)) {
        btstack_hid_usage_item_t item;
        btstack_hid_usage_iterator_get_item(&usages, &item);
        unsigned report_id = item.report_id == HID_REPORT_ID_UNDEFINED ? 0 : item.report_id;
        if (report_id > 255 || !item.size || item.size > 32 ||
            (unsigned)item.bit_pos + item.size > bits[report_id]) return false;
        if ((item.descriptor_item.item_value & 3) != 2) continue; /* Data, Variable. */
        uint8_t axis = 0, button = 0;
        if (item.usage_page == 1 && (item.descriptor_item.item_value & 4) &&
            item.global_logical_minimum < 0) {
            if (item.usage == 0x30) axis = 1;
            if (item.usage == 0x31) axis = 2;
            if (item.usage == 0x38) axis = 3;
        } else if (item.usage_page == 9 && item.usage >= 1 && item.usage <= 8) {
            button = (uint8_t)item.usage;
        }
        if (!axis && !button) continue;
        if (layout->count == HUB_USB_MOUSE_FIELDS) return false;
        layout->fields[layout->count++] = (hub_usb_mouse_field_t) {
            .bit = item.bit_pos, .id = (uint8_t)report_id, .size = item.size,
            .axis = axis, .button = button
        };
        if (axis == 1 || axis == 2) {
            layout->mouse[report_id] = true;
            have_motion = true;
        }
    }
    layout->valid = have_motion;
    return layout->valid;
}

bool hub_usb_mouse_decode(const hub_usb_mouse_layout_t *layout, const uint8_t *report,
                          uint16_t length, hub_usb_mouse_motion_t *motion) {
    memset(motion, 0, sizeof(*motion));
    if (!layout->valid || !report || !length) return false;
    unsigned id = layout->numbered ? report[0] : 0;
    if (!layout->mouse[id] || length != layout->sizes[id]) return false;
    if (layout->numbered) ++report;
    int64_t axes[3] = {0};
    for (unsigned i = 0; i < layout->count; ++i) {
        const hub_usb_mouse_field_t *field = &layout->fields[i];
        if (field->id != id) continue;
        uint32_t value = 0;
        for (unsigned bit = 0; bit < field->size; ++bit) {
            unsigned source = field->bit + bit;
            value |= (uint32_t)((report[source / 8u] >> (source % 8u)) & 1u) << bit;
        }
        if (field->button) {
            if (value) motion->buttons |= (uint8_t)(1u << (field->button - 1u));
        } else {
            int64_t signed_value = value;
            if (value & (UINT32_C(1) << (field->size - 1u)))
                signed_value -= INT64_C(1) << field->size;
            axes[field->axis - 1u] += signed_value;
        }
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (axes[i] < INT32_MIN || axes[i] > INT32_MAX) return false;
    }
    motion->dx = (int32_t)axes[0];
    motion->dy = (int32_t)axes[1];
    motion->wheel = (int32_t)axes[2];
    return true;
}
