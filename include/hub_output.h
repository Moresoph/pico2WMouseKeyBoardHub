#ifndef HUB_OUTPUT_H
#define HUB_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>

void hub_output_init(void);
void hub_output_start(void);
void hub_output_status(void);
bool hub_output_ready(unsigned slot);
unsigned hub_output_active_slot(void);
bool hub_output_switching(void);

/* Call from the BTstack/async-context lock, like the other output APIs.
   Baseline (default) sends once per trigger; fast mode drains a bounded burst. */
void hub_output_set_low_latency(bool enabled);
uint16_t hub_output_active_handle(void);
/* MCU enqueue -> successful ATT notify, not radio delivery. Metrics belong to
   logical PCs across reconnects. Reset keeps queues/state; pre-reset queued
   events (including later merges into them) are excluded from timing samples. */
void hub_output_latency_status(void);
void hub_output_latency_reset(void);

/* A switch releases the previous computer before accepting input on the next. */
bool hub_output_select(unsigned slot);
bool hub_output_keyboard(const uint8_t report[8]);
bool hub_output_mouse(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel);
void hub_output_release_all(void);

#endif
