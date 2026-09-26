#ifndef HUB_PERF_H
#define HUB_PERF_H

#include <stdbool.h>
#include <stdint.h>

/* RAM-only metadata. These calls never log report contents or change radio policy. */
void hub_perf_init(void);
/* Synchronous dump; print spans are retained so induced delays stay identifiable. */
void hub_perf_status(void);
/* Compact passive Classic mode/subrating snapshot, without the full trace dump. */
void hub_perf_classic_status(void);
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

typedef enum {
    HUB_PERF_INPUT_SECURED, HUB_PERF_INPUT_DISCOVERY, HUB_PERF_INPUT_READY,
    HUB_PERF_INPUT_FIRST_HID, HUB_PERF_INPUT_FIRST_ROUTED, HUB_PERF_INPUT_HOGP_DONE,
    HUB_PERF_INPUT_RETRY_WAIT, HUB_PERF_INPUT_CONNECT_START, HUB_PERF_INPUT_CONNECT_FAILED
} hub_perf_input_phase_t;
void hub_perf_input_phase(unsigned input, uint16_t handle, hub_perf_input_phase_t phase, uint8_t status);
/* completed=false status is the local API result; completed=true is the raw ATT status. */
void hub_perf_hids_cccd(uint16_t handle, uint16_t report_handle, bool completed, uint8_t status);

#endif
