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

/* A switch releases the previous computer before accepting input on the next. */
bool hub_output_select(unsigned slot);
bool hub_output_keyboard(const uint8_t report[8]);
bool hub_output_mouse(uint8_t buttons, int32_t dx, int32_t dy, int32_t wheel);
void hub_output_release_all(void);

#endif
