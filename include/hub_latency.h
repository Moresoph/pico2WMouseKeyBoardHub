#ifndef HUB_LATENCY_H
#define HUB_LATENCY_H

#include <stdbool.h>
#include <stdint.h>

void hub_latency_init(void);
bool hub_latency_set_fast(bool enabled);
void hub_latency_status(void);
void hub_latency_reset(void);
uint32_t hub_latency_now_us(void);
void hub_latency_input_report(unsigned input, bool mouse);

#endif
