#ifndef HUB_SECURITY_H
#define HUB_SECURITY_H

#include <stdbool.h>
#include <stdint.h>

bool hub_security_pairing_open(void);
bool hub_security_allow_pairing(uint16_t handle);
bool hub_security_input_pairing_blocked(void);
bool hub_security_capture_active(void);
bool hub_security_keyboard(unsigned input, const uint8_t report[8]);
void hub_security_input_disconnected(unsigned input);

#endif
