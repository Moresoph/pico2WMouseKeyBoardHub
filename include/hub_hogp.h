#ifndef HUB_HOGP_H
#define HUB_HOGP_H

#include <stdint.h>

/* Companion discovery runs after HID readiness; it never authorizes input. */
void hub_hogp_init(void);
void hub_hogp_start(unsigned input, uint16_t handle);
void hub_hogp_stop(unsigned input);
void hub_hogp_tick(void);
void hub_hogp_status(unsigned input);

#endif
