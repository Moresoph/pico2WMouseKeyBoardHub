#include "hub_latency.h"
#include "pico/time.h"

uint32_t hub_latency_now_us(void) {
    return time_us_32();
}
