#ifndef HUB_LATENCY_H
#define HUB_LATENCY_H

#include <stdbool.h>
#include <stdint.h>

void hub_latency_init(void);
bool hub_latency_set_fast(bool enabled);
bool hub_latency_set_mouse_active(bool enabled);
void hub_latency_mouse_active_status(void);
bool hub_latency_set_pc_standby(bool enabled);
void hub_latency_pc_standby_status(void);
void hub_latency_mouse_activity(uint16_t handle);
void hub_latency_status(void);
void hub_latency_reset(void);
uint32_t hub_latency_now_us(void);
void hub_latency_input_report(unsigned input, bool mouse);

#endif
