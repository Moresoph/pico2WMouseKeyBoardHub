#include "hub_output_timer.h"
#include "pico/async_context.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"

static void (*pending_callback)(void);
static bool armed;

static void timer_ready(async_context_t *context, async_at_time_worker_t *worker) {
    (void)context;
    (void)worker;
    void (*callback)(void) = pending_callback;
    pending_callback = NULL;
    armed = false;
    callback();
}

static async_at_time_worker_t worker = { .do_work = timer_ready };

void hub_output_timer_cancel(void) {
    if (armed) async_context_remove_at_time_worker(cyw43_arch_async_context(), &worker);
    armed = false;
    pending_callback = NULL;
}

bool hub_output_timer_schedule_us(uint32_t delay_us, void (*callback)(void)) {
    if (armed || !callback) return false;
    pending_callback = callback;
    armed = true;
    if (async_context_add_at_time_worker_at(cyw43_arch_async_context(), &worker,
                                            delayed_by_us(get_absolute_time(), delay_us))) return true;
    armed = false;
    pending_callback = NULL;
    return false;
}
