#ifndef HUB_ESP_RECONNECT_POLICY_H
#define HUB_ESP_RECONNECT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

enum { INPUT_RECONNECT_TIMEOUT_MS = 40000, INPUT_PROFILE_GRACE_MS = 1000,
       INPUT_HID_TIMEOUT_MS = 15000, INPUT_ENCRYPT_TIMEOUT_MS = 12000,
       INPUT_CLEANUP_TIMEOUT_MS = 20000, INPUT_ATTEMPT_TIMEOUT_MS = 65000 };
typedef enum { INPUT_PHASE_IDLE, INPUT_PHASE_PAGE, INPUT_PHASE_HID, INPUT_PHASE_ENCRYPT,
               INPUT_PHASE_READY, INPUT_PHASE_CLEANUP, INPUT_PHASE_PAUSED } input_link_phase_t;
typedef struct {
    input_link_phase_t phase;
    uint32_t phase_at, attempt_at;
    bool attempting;
} input_link_clock_t;

static inline const char *input_link_phase_name(input_link_phase_t phase) {
    static const char *const names[] = {"idle","paging","hid-setup","encrypting","ready","cleanup","paused"};
    return names[phase];
}

static inline void input_link_enter(input_link_clock_t *clock, input_link_phase_t phase, uint32_t now) {
    if (clock->phase != phase) { clock->phase = phase; clock->phase_at = now; }
    if (phase == INPUT_PHASE_PAGE || phase == INPUT_PHASE_HID || phase == INPUT_PHASE_ENCRYPT) {
        if (!clock->attempting) { clock->attempting = true; clock->attempt_at = now; clock->phase_at = now; }
    } else clock->attempting = false;
}

static inline bool input_link_expired(const input_link_clock_t *clock, uint32_t now, bool pairing) {
    if (clock->phase == INPUT_PHASE_CLEANUP) return now - clock->phase_at >= INPUT_CLEANUP_TIMEOUT_MS;
    if (!clock->attempting) return false;
    if (now - clock->attempt_at >= (pairing ? 120000u : INPUT_ATTEMPT_TIMEOUT_MS)) return true;
    uint32_t limit = clock->phase == INPUT_PHASE_PAGE ? INPUT_RECONNECT_TIMEOUT_MS :
        clock->phase == INPUT_PHASE_HID ? INPUT_HID_TIMEOUT_MS :
        clock->phase == INPUT_PHASE_ENCRYPT ? (pairing ? 120000u : INPUT_ENCRYPT_TIMEOUT_MS) : UINT32_MAX;
    return now - clock->phase_at >= limit;
}

static inline bool input_scan_blocks_transport(bool classic, unsigned requested, unsigned active, bool scanning) {
    unsigned transport = classic ? 1u : 2u;
    return requested == transport || (scanning && (active == 0 || active == transport));
}
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
