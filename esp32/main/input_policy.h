#ifndef ESP_INPUT_POLICY_H
#define ESP_INPUT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

static inline bool input_pairing_window_open(bool selected, bool paused,
                                             uint32_t now, uint32_t until) {
    return selected && !paused && until && (int32_t)(now - until) < 0;
}

static inline bool input_classic_admitted(bool selected, bool paused, bool bonded,
                                          bool pairing_window, bool allow_open) {
    return selected && !paused && (bonded || pairing_window || allow_open);
}

#endif
