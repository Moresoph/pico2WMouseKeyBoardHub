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

static inline bool input_classic_acl_pending(bool selected, bool paused, bool pending,
                                             bool incoming, bool has_device, bool acl_connected) {
    return selected && !paused && pending && incoming && !has_device && acl_connected;
}

static inline bool input_classic_encryption_current(bool selected, bool paused, bool connected,
                                                    bool mapped, bool requested,
                                                    uint32_t generation, uint32_t reply_generation) {
    return selected && !paused && connected && mapped && requested && generation == reply_generation;
}

#endif
