#include "hub_external_input.h"
#include "hub_router.h"
#include <string.h>

void hub_external_reset(hub_external_input_t *s, uint32_t nonce) {
    for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i) hub_router_input_disconnected(i);
    s->nonce = nonce;
    s->online = false;
    s->session = 0;
    ++s->resets;
    memset(s->inputs, 0, sizeof(s->inputs));
}

bool hub_external_expired(const hub_external_input_t *s, uint32_t now) {
    return s->online && (uint32_t)(now - s->last_rx) >= HUB_LINK_TIMEOUT_MS;
}

bool hub_external_receive(hub_external_input_t *s, const hub_link_frame_t *f, uint32_t now) {
    if (f->type != HUB_LINK_STATE || f->nonce != s->nonce) return true;
    hub_link_input_t next[HUB_LINK_SLOTS];
    if (!f->session || !hub_link_unpack(f, next)) return false;
    if (s->online) {
        if (f->session != s->session) return false;
        if (f->sequence == s->sequence) return true; /* Retransmission must not replay motion. */
        if (f->sequence != (uint16_t)(s->sequence + 1u)) return false;
        if (hub_external_expired(s, now)) return false;
    }
    for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i) {
        bool changed = !s->online || next[i].epoch != s->inputs[i].epoch ||
            next[i].roles != s->inputs[i].roles;
        if (changed) {
            if (next[i].roles) hub_router_input_connected(i);
            else hub_router_input_disconnected(i);
        }
        if (next[i].roles & HUB_LINK_KEYBOARD)
            hub_router_keyboard(i, next[i].keyboard);
        if (next[i].roles & HUB_LINK_MOUSE)
            hub_router_mouse(i, next[i].buttons, changed ? 0 : next[i].dx,
                             changed ? 0 : next[i].dy, changed ? 0 : next[i].wheel);
    }
    memcpy(s->inputs, next, sizeof(next));
    s->session = f->session;
    s->sequence = f->sequence;
    s->last_rx = now;
    s->online = true;
    ++s->accepted;
    return true;
}
