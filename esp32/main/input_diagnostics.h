#ifndef HUB_INPUT_DIAGNOSTICS_H
#define HUB_INPUT_DIAGNOSTICS_H

#include <stdint.h>
#include <string.h>

enum { HUB_INPUT_HISTORY = 24 };
typedef enum { INPUT_EVENT_OPEN, INPUT_EVENT_ACL_UP, INPUT_EVENT_HID_OPEN, INPUT_EVENT_ENCRYPT,
               INPUT_EVENT_HID_CLOSE, INPUT_EVENT_ACL_DOWN, INPUT_EVENT_OPEN_FAILED,
               INPUT_EVENT_RECOVER, INPUT_EVENT_CLEANUP_RESULT, INPUT_EVENT_READY,
               INPUT_EVENT_PAUSED, INPUT_EVENT_RETRY, INPUT_EVENT_FIRST_REPORT,
               INPUT_EVENT_DESCRIPTOR_RETRY } input_event_kind_t;
typedef struct {
    uint32_t at, detail;
    uint8_t slot, phase, kind;
} input_event_t;
typedef struct {
    input_event_t events[HUB_INPUT_HISTORY];
    unsigned count, head;
} input_history_t;

static inline const char *input_event_name(input_event_kind_t kind) {
    static const char *const names[] = {"open","acl-up","hid-open","encryption","hid-close","acl-down",
        "open-failed","recover","cleanup-result","ready","paused","operator-retry","first-report","descriptor-retry"};
    return names[kind];
}
static inline void input_history_add(input_history_t *history, const input_event_t *event) {
    history->events[history->head] = *event;
    history->head = (history->head + 1u) % HUB_INPUT_HISTORY;
    if (history->count < HUB_INPUT_HISTORY) ++history->count;
}
static inline void input_history_snapshot(const input_history_t *history, input_event_t *out) {
    for (unsigned i = 0; i < history->count; ++i)
        out[i] = history->events[(history->head + HUB_INPUT_HISTORY - history->count + i) % HUB_INPUT_HISTORY];
}
#endif
