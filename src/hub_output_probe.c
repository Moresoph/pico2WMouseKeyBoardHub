#include <inttypes.h>
#include <stdio.h>

#include "hub_latency.h"
#include "hub_output.h"
#include "hub_output_probe.h"

#define PROBE_DURATION_US 30000000u

static bool running;
static unsigned target_slot;
static uint32_t period, started_us, next_us, generated, skipped, max_late_us, elapsed_us;

bool hub_output_probe_active(void) { return running; }

void hub_output_probe_status(void) {
    printf("[output-test] active=%u period-us=%" PRIu32 " duration-us=%u"
           " elapsed-us=%" PRIu32 " generated=%" PRIu32 " skipped=%" PRIu32
           " max-generator-late-us=%" PRIu32 " PC=%u\n",
           running, period, PROBE_DURATION_US,
           running ? hub_latency_now_us() - started_us : elapsed_us,
           generated, skipped, max_late_us, target_slot + 1);
}

static void finish(const char *reason) {
    running = false;
    elapsed_us = hub_latency_now_us() - started_us;
    hub_output_probe_cancel_timer();
    /* Discard any unsent test motion and leave no buttons or keys held. */
    hub_output_release_all();
    printf("[output-test] stopped: %s\n", reason);
    hub_output_probe_status();
}

void hub_output_probe_stop(void) {
    if (running) finish("manual stop");
    else printf("[output-test] already stopped\n");
}

bool hub_output_probe_start(uint32_t period_us) {
    unsigned slot = hub_output_active_slot();
    if (running || (period_us != 2500u && period_us != 7500u)
        || !hub_output_probe_allowed() || !hub_output_ready(slot)) {
        printf("[output-test] rejected: use 2500/7500us, only the selected ready PC,"
               " no scanning/reconnect/pairing, and no running test\n");
        return false;
    }
    target_slot = slot;
    period = period_us;
    generated = skipped = max_late_us = elapsed_us = 0;
    printf("[output-test] starting 30s motion-only test; no clicks/keys;"
           " release or output-test stop aborts; normal output pacing unchanged\n");
    started_us = hub_latency_now_us();
    next_us = started_us + period;
    running = true;
    if (!hub_output_probe_schedule_us(period)) {
        finish("timer scheduling failed");
        return false;
    }
    return true;
}

void hub_output_probe_poll(void) {
    if (!running) return;
    uint32_t now = hub_latency_now_us();
    if ((uint32_t)(now - started_us) >= PROBE_DURATION_US) {
        finish("duration reached");
        return;
    }
    if (!hub_output_probe_allowed() || hub_output_active_slot() != target_slot
        || !hub_output_ready(target_slot)) {
        finish("PC unavailable or input/radio activity");
        return;
    }
    int32_t late = (int32_t)(now - next_us);
    if (late >= 0) {
        if ((uint32_t)late > max_late_us) max_late_us = (uint32_t)late;
        uint32_t missed = (uint32_t)late / period;
        skipped += missed;
        next_us += (missed + 1u) * period;
        /* Bounded 64-count horizontal travel; normal motion merging stays on. */
        int32_t dx = (generated / 64u) & 1u ? -1 : 1;
        if (!hub_output_mouse(0, dx, 0, 0)) {
            finish("motion rejected");
            return;
        }
        ++generated;
    }
    now = hub_latency_now_us();
    int32_t remaining = (int32_t)(next_us - now);
    if (!hub_output_probe_schedule_us(remaining > 0 ? (uint32_t)remaining : 1u))
        finish("timer scheduling failed");
}
