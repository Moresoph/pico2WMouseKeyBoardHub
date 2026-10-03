#ifndef HUB_AUTO_INPUT_POLICY_H
#define HUB_AUTO_INPUT_POLICY_H

#include "hub_web.h"

static inline bool hub_input_is_hub_name(const char *name) {
    return !strncmp(name, "Pico2W ", 7) || !strncmp(name, "Pico Dual BT", 12) ||
        !strncmp(name, "PicoHub", 7);
}

static inline int hub_auto_candidate(const hub_web_candidate_t *candidates, unsigned count,
                                     const hub_input_records_t *saved, unsigned role) {
    int selected = -1;
    for (unsigned i = 0; i < count; ++i) {
        const hub_web_candidate_t *candidate = &candidates[i];
        if (!candidate->hid || candidate->roles != role || hub_input_is_hub_name(candidate->name)) continue;
        bool known = false;
        for (unsigned j = 0; j < HUB_SAVED_INPUTS; ++j)
            known |= saved->slots[j].used && !memcmp(saved->slots[j].address, candidate->address, 6);
        if (known) continue;
        if (selected >= 0) return -2;
        selected = (int)i;
    }
    return selected;
}

#endif
