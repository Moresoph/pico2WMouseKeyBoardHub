#ifndef HUB_CLASSIC_SECURITY_H
#define HUB_CLASSIC_SECURITY_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t hub_esp_classic_encrypt(const uint8_t address[6], uint32_t generation);
typedef enum { HUB_CLEANUP_CURRENT, HUB_CLEANUP_DEFERRED, HUB_CLEANUP_STALE } hub_cleanup_guard_t;
typedef enum { HUB_CLEANUP_SENT, HUB_CLEANUP_LOCK_BUSY, HUB_CLEANUP_OBSOLETE,
               HUB_CLEANUP_ABSENT, HUB_CLEANUP_FAILED } hub_cleanup_result_t;
esp_err_t hub_esp_classic_close_acl(const uint8_t address[6], uint16_t handle, uint32_t generation,
                                   uint32_t token);
hub_cleanup_guard_t hub_esp_classic_recovery_check(const uint8_t address[6], uint16_t handle,
                                                  uint32_t generation, uint32_t token);
void hub_esp_classic_cleanup_result(const uint8_t address[6], uint16_t handle, uint32_t generation,
                                    uint32_t token, hub_cleanup_result_t result, uint8_t status);
void hub_esp_classic_encryption_result(const uint8_t address[6], uint32_t generation,
                                      bool connected, bool encrypted, bool security_failure,
                                      uint8_t status, uint8_t flags);

#endif
