#ifndef HUB_CLASSIC_SECURITY_H
#define HUB_CLASSIC_SECURITY_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t hub_esp_classic_encrypt(const uint8_t address[6], uint32_t generation);
esp_err_t hub_esp_classic_close_acl(const uint8_t address[6], uint16_t handle, uint32_t generation);
bool hub_esp_classic_recovery_current(const uint8_t address[6], uint16_t handle, uint32_t generation);
void hub_esp_classic_encryption_result(const uint8_t address[6], uint32_t generation,
                                      bool connected, bool encrypted, bool security_failure,
                                      uint8_t status, uint8_t flags);

#endif
