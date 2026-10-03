#ifndef HUB_WEB_H
#define HUB_WEB_H

#include "esp_err.h"
#include "hub_link.h"
#include "hub_input_records.h"

#define HUB_WEB_FIRMWARE "0.6.3-auto-input"
#define HUB_WEB_CANDIDATES 16u
typedef struct {
    uint8_t address[6], transport, type, roles;
    bool hid;
    char name[64];
} hub_web_candidate_t;
typedef struct {
    char action[16], token[17], transport[8], address[18], value[8];
    char name[HUB_INPUT_NAME_BYTES];
    unsigned slot, candidate;
    bool replace;
} hub_web_command_t;

typedef struct {
    bool saved, bonded, connected, encrypted, hid, ready, paused, pending;
    uint8_t transport, roles, address[6], record_slot;
    uint16_t interval;
    uint32_t reports;
    char name[256];
} hub_web_input_t;

typedef struct {
    uint32_t uptime_ms, packets, overflows, pico_age_ms;
    bool uart_online, pico_received, pico_fresh;
    hub_web_input_t inputs[HUB_LINK_SLOTS];
    hub_link_output_t output;
    hub_input_records_t records;
    bool management_open, scanning, scan_waiting, trial, records_fault, auto_enrollment;
    uint32_t remaining_ms, auth_number;
    char token[17], operation[192];
    uint8_t trial_role, trial_phase, trial_address[6];
    uint8_t auth, auth_slot, candidate_count;
    hub_web_candidate_t candidates[HUB_WEB_CANDIDATES];
} hub_web_snapshot_t;

esp_err_t hub_web_init(bool (*snapshot)(hub_web_snapshot_t *),
                       esp_err_t (*command)(const hub_web_command_t *));
esp_err_t hub_web_set_enabled(bool enabled);
bool hub_web_enabled(void);
int hub_web_command(int argc, char **argv);

#endif
