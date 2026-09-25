#ifndef HUB_OUTPUT_TIMER_H
#define HUB_OUTPUT_TIMER_H

#include <stdbool.h>
#include <stdint.h>

/* One-shot callback serialized with BTstack, not run from a raw timer IRQ. */
bool hub_output_timer_schedule_us(uint32_t delay_us, void (*callback)(void));
void hub_output_timer_cancel(void);

#endif
