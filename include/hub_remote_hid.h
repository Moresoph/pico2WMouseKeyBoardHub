#ifndef HUB_REMOTE_HID_H
#define HUB_REMOTE_HID_H

#include "hub_link.h"

#define HUB_REMOTE_MAPS 3u
#define HUB_REMOTE_REPORTS 16u
typedef struct {
    const uint8_t *data;
    uint16_t length;
    bool numbered;
} hub_remote_map_t;
typedef struct {
    uint8_t map, id, roles, keys[32], buttons;
    uint16_t bits;
} hub_remote_report_t;
typedef struct {
    hub_remote_map_t maps[HUB_REMOTE_MAPS];
    hub_remote_report_t reports[HUB_REMOTE_REPORTS];
    unsigned count;
    uint8_t roles;
} hub_remote_hid_t;

/* Maps are borrowed until the device closes. IDF strips the report ID from data. */
bool hub_remote_hid_map(hub_remote_hid_t *hid, unsigned map, const uint8_t *data, uint16_t length);
bool hub_remote_hid_report(hub_remote_hid_t *hid, unsigned map, unsigned id,
                           const uint8_t *data, size_t length, hub_link_input_t *state);

#endif
