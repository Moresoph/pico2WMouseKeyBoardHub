#ifndef HUB_DIAG_RING_H
#define HUB_DIAG_RING_H

#include <stdbool.h>

#define HUB_DIAG_LINES 40u
#define HUB_DIAG_LINE_BYTES 192u

typedef struct {
    char lines[HUB_DIAG_LINES][HUB_DIAG_LINE_BYTES];
    unsigned next;
    unsigned count;
} hub_diag_ring_t;

void hub_diag_ring_init(hub_diag_ring_t *ring);
bool hub_diag_ring_push(hub_diag_ring_t *ring, const char *line);
const char *hub_diag_ring_get(const hub_diag_ring_t *ring, unsigned index);

#endif
