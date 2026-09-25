#include <assert.h>
#include <stdio.h>
#include <stdint.h>

#include "..\src\hub_output_probe.c"

static uint32_t clock_us, due_us;
static bool allowed = true, ready = true, timer_ok = true, send_ok = true, armed;
static unsigned motions, releases;
static unsigned selected_slot;
static int32_t position;

uint32_t hub_latency_now_us(void) { return clock_us; }
bool hub_output_probe_allowed(void) { return allowed; }
unsigned hub_output_active_slot(void) { return selected_slot; }
bool hub_output_ready(unsigned slot) { assert(slot == selected_slot); return ready; }
bool hub_output_probe_schedule_us(uint32_t delay) {
    assert(!armed && delay > 0);
    if (!timer_ok) return false;
    due_us = clock_us + delay;
    armed = true;
    return true;
}
void hub_output_probe_cancel_timer(void) { armed = false; }
void hub_output_release_all(void) { ++releases; }
bool hub_output_mouse(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel) {
    assert(buttons == 0 && dy == 0 && wheel == 0 && (dx == 1 || dx == -1));
    if (!send_ok) return false;
    position += dx;
    assert(position >= 0 && position <= 64);
    ++motions;
    return true;
}
static void fire(uint32_t late_us) {
    assert(armed);
    clock_us = due_us + late_us;
    armed = false;
    hub_output_probe_poll();
}
static void reset(uint32_t at) {
    assert(!hub_output_probe_active() && !armed);
    clock_us = at;
    allowed = ready = timer_ok = send_ok = true;
    motions = releases = 0;
    selected_slot = 0;
    position = 0;
}
int main(void) {
    reset(0);
    assert(!hub_output_probe_start(0));
    assert(!hub_output_probe_start(5000));
    allowed = false;
    assert(!hub_output_probe_start(2500));
    allowed = true; ready = false;
    assert(!hub_output_probe_start(2500));
    ready = true; timer_ok = false;
    assert(!hub_output_probe_start(2500));
    assert(releases == 1 && !armed);

    for (unsigned i = 0; i < 2; ++i) {
        reset(i ? UINT32_MAX - 10000u : 0);
        uint32_t step = i ? 7500 : 2500;
        assert(hub_output_probe_start(step));
        assert(!hub_output_probe_start(step));
        while (hub_output_probe_active()) fire(0);
        assert(motions == PROBE_DURATION_US / step - 1);
        assert(elapsed_us == PROBE_DURATION_US && skipped == 0);
        assert(releases == 1 && !armed);
    }

    reset(0);
    assert(hub_output_probe_start(2500));
    fire(8000);
    assert(motions == 1 && skipped == 3 && max_late_us == 8000);
    assert(due_us == 12500);
    hub_output_probe_stop();
    assert(!armed && releases == 1);
    hub_output_probe_poll();
    assert(motions == 1);
    position = 0;
    assert(hub_output_probe_start(7500));
    fire(0);
    allowed = false;
    fire(0);
    assert(!hub_output_probe_active() && !armed && releases == 2);

    reset(0);
    assert(hub_output_probe_start(2500));
    ready = false;
    fire(0);
    assert(motions == 0 && releases == 1 && !armed);

    reset(0);
    selected_slot = 2;
    assert(hub_output_probe_start(7500));
    fire(0);
    assert(motions == 1 && target_slot == 2);
    selected_slot = 1;
    fire(0);
    assert(motions == 1 && releases == 1 && !armed);

    reset(0);
    assert(hub_output_probe_start(2500));
    send_ok = false;
    fire(0);
    assert(motions == 0 && releases == 1 && !armed);

    reset(0);
    assert(hub_output_probe_start(2500));
    timer_ok = false;
    fire(0);
    assert(motions == 1 && releases == 1 && !armed);
    puts("Output probe tests passed");
    return 0;
}
