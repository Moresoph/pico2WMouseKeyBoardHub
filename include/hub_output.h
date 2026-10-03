#ifndef HUB_OUTPUT_H
#define HUB_OUTPUT_H

#include <stdbool.h>
#include <stdint.h>
#include "hub_link.h"

void hub_output_init(void);
void hub_output_start(void);
void hub_output_status(void);
bool hub_output_ready(unsigned slot);
unsigned hub_output_active_slot(void);
bool hub_output_switching(void);
void hub_output_snapshot(hub_link_output_t *snapshot);
bool hub_output_pairing_prepare(void);
bool hub_output_forget(unsigned slot, uint8_t type, const uint8_t address[6]);
bool hub_output_slot_saved(unsigned slot);
bool hub_output_storage_healthy(void);

/* Call from the BTstack/async-context lock, like the other output APIs.
   Baseline (default) sends once per trigger; fast mode drains a bounded burst. */
void hub_output_set_low_latency(bool enabled);
/* BLE-only RAM experiment, on at boot; independent of latency mode. */
bool hub_output_set_mouse_buffering(bool enabled);
void hub_output_mouse_buffer_status(void);
uint16_t hub_output_active_handle(void);
bool hub_output_is_ready_connection(uint16_t handle);
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
