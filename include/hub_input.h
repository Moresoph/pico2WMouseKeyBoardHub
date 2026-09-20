#ifndef HUB_INPUT_H
#define HUB_INPUT_H

#include <stdbool.h>
#include <stdint.h>

void hub_input_init(void);
void hub_input_start(void);
void hub_input_status(void);
void hub_input_scan(bool classic);
void hub_input_scan_stop(void);

/* Address text uses AA:BB:CC:DD:EE:FF. BLE type is public (0) or random (1). */
bool hub_input_connect(unsigned slot, bool classic, uint8_t address_type,
                       const char *address);
void hub_input_disconnect(unsigned slot);
bool hub_input_is_connection(uint16_t handle);
bool hub_input_is_expected_classic(const uint8_t address[6]);
bool hub_input_allow_pairing(uint16_t handle);
bool hub_input_keyboard_ready(void);
bool hub_input_pairing_busy(void);

/* Enrollment fills missing keyboard/mouse roles; it never replaces stored peers. */
bool hub_input_enroll_start(void);
void hub_input_enroll_stop(void);
bool hub_input_enrolling(void);

/* Bit 0 = input 1, bit 1 = input 2. Forget only these inputs, then enroll replacements. */
bool hub_input_forget(unsigned slot_mask);
bool hub_input_open_test_connect(unsigned slot, const char *address);
bool hub_input_open_test_peer(const uint8_t address[6]);
bool hub_input_open_bridge_enabled(void);
bool hub_input_set_open_bridge(bool enabled);

#endif
