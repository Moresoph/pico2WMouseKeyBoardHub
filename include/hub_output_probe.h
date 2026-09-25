#ifndef HUB_OUTPUT_PROBE_H
#define HUB_OUTPUT_PROBE_H

#include <stdbool.h>
#include <stdint.h>

bool hub_output_probe_start(uint32_t period_us);
void hub_output_probe_stop(void);
void hub_output_probe_poll(void);
void hub_output_probe_status(void);
bool hub_output_probe_active(void);

/* Platform hooks use a dedicated timer serialized with BTstack. */
bool hub_output_probe_allowed(void);
bool hub_output_probe_schedule_us(uint32_t delay_us);
void hub_output_probe_cancel_timer(void);

#endif
