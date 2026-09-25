#ifndef HUB_PERF_H
#define HUB_PERF_H

#include <stdbool.h>
#include <stdint.h>

/* RAM-only metadata. These calls never log report contents or change radio policy. */
void hub_perf_init(void);
/* Synchronous dump; print spans are retained so induced delays stay identifiable. */
void hub_perf_status(void);
/* Clear measurements, preserving active link identities and radio/application state. */
void hub_perf_reset(void);
void hub_perf_input_report(unsigned input, bool mouse);
/* Called before BTstack rewrites RX headers. Counts metadata, never stores payloads. */
void hub_perf_acl_receive(const uint8_t *packet, uint16_t size);
void hub_perf_acl_transmit(const uint8_t *packet, uint16_t size);
/* Queue wait through successful ATT submission, NOT controller/peer completion. */
void hub_perf_output_wait(uint16_t handle, unsigned slot, bool mouse, uint32_t wait_us);
void hub_perf_parameter_request(uint16_t handle, bool fast, uint16_t min_interval,
                                uint16_t max_interval, uint16_t peripheral_latency,
                                uint16_t timeout, int status);
void hub_perf_parameter_timeout(uint16_t handle);

#endif
