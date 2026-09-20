#include "hub_diag_ring.h"
#include <stddef.h>
#include <string.h>

void hub_diag_ring_init(hub_diag_ring_t *ring) {
    memset(ring, 0, sizeof(*ring));
}

bool hub_diag_ring_push(hub_diag_ring_t *ring, const char *line) {
    if (!ring || !line) return false;
    char *destination = ring->lines[ring->next];
    size_t length = strlen(line);
    if (length >= HUB_DIAG_LINE_BYTES) {
        memcpy(destination, line, HUB_DIAG_LINE_BYTES - 4u);
        memcpy(destination + HUB_DIAG_LINE_BYTES - 4u, "...", 4);
    } else {
        memcpy(destination, line, length + 1u);
    }
    ring->next = (ring->next + 1u) % HUB_DIAG_LINES;
    if (ring->count < HUB_DIAG_LINES) ++ring->count;
    return true;
}

const char *hub_diag_ring_get(const hub_diag_ring_t *ring, unsigned index) {
    if (!ring || index >= ring->count) return NULL;
    return ring->lines[(ring->next + HUB_DIAG_LINES - ring->count + index) % HUB_DIAG_LINES];
}
