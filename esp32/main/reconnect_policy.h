#ifndef HUB_ESP_RECONNECT_POLICY_H
#define HUB_ESP_RECONNECT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

enum { INPUT_RECONNECT_TIMEOUT_MS = 40000, INPUT_PROFILE_GRACE_MS = 5000 };
typedef enum { INPUT_WAIT, INPUT_OPEN, INPUT_ATTACH, INPUT_RECOVER } input_reconnect_action_t;
typedef struct {
    bool selected, paused, pending, recovering, opening, device, mapped, forwarding;
    bool classic, acl, profile_requested, pairing;
    uint32_t started_at, retry_at;
} input_reconnect_view_t;

static inline bool input_reconnect_busy(const input_reconnect_view_t *p) {
    if (p->opening || p->recovering) return true;
    return p->pending && !(p->classic && p->acl && !p->profile_requested && !p->mapped);
}

static inline input_reconnect_action_t input_reconnect_plan(const input_reconnect_view_t *p,
                                                            uint32_t now, bool transport_busy) {
    if (!p->selected || p->paused || p->recovering || (p->mapped && p->forwarding)) return INPUT_WAIT;
    uint32_t age = now - p->started_at;
    if (p->pending && age >= INPUT_RECONNECT_TIMEOUT_MS && !p->pairing) return INPUT_RECOVER;
    if (p->opening || transport_busy) return INPUT_WAIT;
    if (p->classic && p->acl && !p->mapped && !p->profile_requested &&
        age >= INPUT_PROFILE_GRACE_MS) return INPUT_ATTACH;
    if (!p->pending && !p->acl && !p->device && !p->mapped &&
        (int32_t)(now - p->retry_at) >= 0) return INPUT_OPEN;
    return INPUT_WAIT;
}

static inline uint32_t input_reconnect_backoff(unsigned failures) {
    if (!failures) return 1000;
    return failures >= 5 ? 15000 : 1000u << (failures - 1);
}

#endif
