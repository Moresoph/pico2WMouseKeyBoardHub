#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hub_diag_ring.h"

int main(void) {
    hub_diag_ring_t ring;
    hub_diag_ring_init(&ring);
    assert(ring.count == 0 && hub_diag_ring_get(&ring, 0) == NULL);
    for (unsigned i = 0; i < HUB_DIAG_LINES + 5u; ++i) {
        char line[32];
        snprintf(line, sizeof(line), "event-%u", i);
        assert(hub_diag_ring_push(&ring, line));
    }
    assert(ring.count == HUB_DIAG_LINES);
    assert(strcmp(hub_diag_ring_get(&ring, 0), "event-5") == 0);
    assert(strcmp(hub_diag_ring_get(&ring, HUB_DIAG_LINES - 1u), "event-44") == 0);
    assert(hub_diag_ring_get(&ring, HUB_DIAG_LINES) == NULL);
    char long_line[HUB_DIAG_LINE_BYTES + 20u];
    memset(long_line, 'x', sizeof(long_line));
    long_line[sizeof(long_line) - 1u] = 0;
    assert(hub_diag_ring_push(&ring, long_line));
    const char *last = hub_diag_ring_get(&ring, HUB_DIAG_LINES - 1u);
    assert(strlen(last) == HUB_DIAG_LINE_BYTES - 1u);
    assert(strcmp(last + HUB_DIAG_LINE_BYTES - 4u, "...") == 0);
    assert(!hub_diag_ring_push(&ring, NULL));
    puts("All diagnostic ring ordering/bounds tests passed.");
    return 0;
}
