#include "hub_hogp.h"
#include "hub_config.h"
#include "hub_perf.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "btstack_config.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "bluetooth_gatt.h"
#include "ble/gatt_client.h"
#include "gap.h"

#define HOGP_PHASE_TIMEOUT_MS 30000u
#define HOGP_HID_SERVICES 3u

typedef enum {
    HOGP_DIS_SERVICE, HOGP_DIS_CHARACTERISTICS, HOGP_PNP_READ, HOGP_HID_INFO,
    HOGP_SCAN_SERVICE, HOGP_SCAN_CHARACTERISTICS, HOGP_SCAN_CCCD, HOGP_SCAN_WRITE, HOGP_DONE
} hogp_phase_t;

typedef struct {
    bool active, pending, listening, refresh_pending, degraded, bad_value, pnp_valid;
    bool scan_present, scan_checked, scan_subscribed, scan_usable;
    uint16_t handle;
    hogp_phase_t phase;
    uint32_t deadline, refreshes, writes, errors;
    uint8_t service_count, info_count, pnp_source;
    uint16_t vendor, product, version;
    uint16_t info_handles[HOGP_HID_SERVICES];
    uint8_t info_flags[HOGP_HID_SERVICES];
    gatt_client_service_t service;
    gatt_client_characteristic_t pnp, scan, refresh;
    gatt_client_notification_t listener;
} hogp_peer_t;

static hogp_peer_t peers[HUB_INPUT_COUNT];
static const char *const phase_names[] = {
    "DIS-service", "DIS-characteristics", "PnP-read", "HID-information",
    "Scan-service", "Scan-characteristics", "Scan-Refresh-CCCD", "Scan-Interval-Window-write", "done"
};
static void hogp_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static unsigned peer_index(const hogp_peer_t *peer) { return (unsigned)(peer - peers); }

static void set_phase(hogp_peer_t *peer, hogp_phase_t phase) {
    peer->phase = phase;
    peer->pending = false;
    peer->bad_value = false;
    peer->deadline = btstack_run_loop_get_time_ms() + HOGP_PHASE_TIMEOUT_MS;
    if (phase == HOGP_SCAN_SERVICE) {
        peer->service_count = 0;
        memset(&peer->service, 0, sizeof(peer->service));
    }
}

static void stop_listener(hogp_peer_t *peer) {
    if (!peer->listening) return;
    gatt_client_stop_listening_for_characteristic_value_updates(&peer->listener);
    peer->listening = false;
    peer->scan_subscribed = false;
}

static void finish(hogp_peer_t *peer) {
    set_phase(peer, HOGP_DONE);
    hub_perf_input_phase(peer_index(peer), peer->handle, HUB_PERF_INPUT_HOGP_DONE,
                         peer->degraded ? ERROR_CODE_UNSPECIFIED_ERROR : ERROR_CODE_SUCCESS);
}

static void failure(hogp_peer_t *peer, const char *domain, uint8_t status) {
    ++peer->errors;
    peer->degraded = true;
    printf("[hogp %u] handle=0x%04x %s failed: %s=0x%02x; HID forwarding is independent\n",
           peer_index(peer) + 1u, peer->handle, phase_names[peer->phase], domain, status);
    if (peer->phase <= HOGP_PNP_READ) {
        peer->pnp_valid = false;
        set_phase(peer, HOGP_HID_INFO);
    } else if (peer->phase == HOGP_HID_INFO) {
        peer->info_count = 0;
        set_phase(peer, HOGP_SCAN_SERVICE);
    }
    else {
        stop_listener(peer);
        peer->scan_usable = peer->refresh_pending = false;
        finish(peer);
    }
}

static hogp_peer_t *peer_for_handle(uint16_t handle) {
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        if (peers[i].active && peers[i].handle == handle) return &peers[i];
    }
    return NULL;
}

static void completed(hogp_peer_t *peer, uint8_t status) {
    peer->pending = false;
    if (status) {
        failure(peer, "ATT", status);
        return;
    }
    if (peer->bad_value) {
        failure(peer, "invalid-value", ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
        return;
    }
    switch (peer->phase) {
        case HOGP_DIS_SERVICE:
            if (peer->service_count != 1) {
                failure(peer, "service-count", peer->service_count);
                return;
            }
            set_phase(peer, HOGP_DIS_CHARACTERISTICS);
            break;
        case HOGP_DIS_CHARACTERISTICS:
            if (!peer->pnp.value_handle || !(peer->pnp.properties & ATT_PROPERTY_READ)) {
                failure(peer, "missing-readable-PnP", ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                return;
            }
            set_phase(peer, HOGP_PNP_READ);
            break;
        case HOGP_PNP_READ:
            if (!peer->pnp_valid) {
                failure(peer, "missing-PnP-value", ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                return;
            }
            set_phase(peer, HOGP_HID_INFO);
            break;
        case HOGP_HID_INFO:
            if (!peer->info_count) {
                failure(peer, "missing-HID-information", ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                return;
            }
            set_phase(peer, HOGP_SCAN_SERVICE);
            break;
        case HOGP_SCAN_SERVICE:
            peer->scan_checked = true;
            peer->scan_present = peer->service_count != 0;
            if (!peer->service_count) {
                finish(peer); /* Scan Parameters Service is optional on the HID device. */
            } else if (peer->service_count != 1) {
                failure(peer, "service-count", peer->service_count);
            } else set_phase(peer, HOGP_SCAN_CHARACTERISTICS);
            break;
        case HOGP_SCAN_CHARACTERISTICS:
            if (!peer->scan.value_handle || !(peer->scan.properties & ATT_PROPERTY_WRITE_WITHOUT_RESPONSE)) {
                failure(peer, "missing-writable-Scan-Interval-Window", ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                return;
            }
            peer->scan_usable = true;
            if (peer->refresh.value_handle) {
                if (!(peer->refresh.properties & ATT_PROPERTY_NOTIFY)) {
                    failure(peer, "invalid-Scan-Refresh-properties", ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                    return;
                }
                set_phase(peer, HOGP_SCAN_CCCD);
            } else set_phase(peer, HOGP_SCAN_WRITE);
            break;
        case HOGP_SCAN_CCCD:
            peer->scan_subscribed = true;
            set_phase(peer, HOGP_SCAN_WRITE);
            break;
        default:
            failure(peer, "unexpected-completion", ERROR_CODE_COMMAND_DISALLOWED);
            break;
    }
}

static void hogp_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type != HCI_EVENT_PACKET || size < 4) return;
    hogp_peer_t *peer = peer_for_handle(little_endian_read_16(packet, 2));
    if (!peer) return;
    if (packet[0] == GATT_EVENT_NOTIFICATION) {
        if (!peer->listening || size < 12 ||
            gatt_event_notification_get_value_handle(packet) != peer->refresh.value_handle) return;
        if (gatt_event_notification_get_value_length(packet) != 1 || size != 13 || packet[12] != 0) {
            /* Reserved Scan Refresh values are ignored, not treated as input data. */
            ++peer->errors;
            printf("[hogp %u] invalid/reserved Scan Refresh ignored\n", peer_index(peer) + 1u);
            return;
        }
        ++peer->refreshes;
        peer->refresh_pending = true;
        return;
    }
    if (!peer->pending) return;
    switch (packet[0]) {
        case GATT_EVENT_SERVICE_QUERY_RESULT:
            if (size < 28 || (peer->phase != HOGP_DIS_SERVICE && peer->phase != HOGP_SCAN_SERVICE)) {
                peer->bad_value = true;
                break;
            }
            if (peer->service_count < UINT8_MAX) ++peer->service_count;
            if (peer->service_count == 1) gatt_event_service_query_result_get_service(packet, &peer->service);
            break;
        case GATT_EVENT_CHARACTERISTIC_QUERY_RESULT: {
            if (size < 32) { peer->bad_value = true; break; }
            gatt_client_characteristic_t characteristic;
            gatt_event_characteristic_query_result_get_characteristic(packet, &characteristic);
            gatt_client_characteristic_t *target = NULL;
            if (peer->phase == HOGP_DIS_CHARACTERISTICS && characteristic.uuid16 == ORG_BLUETOOTH_CHARACTERISTIC_PNP_ID)
                target = &peer->pnp;
            if (peer->phase == HOGP_SCAN_CHARACTERISTICS) {
                if (characteristic.uuid16 == ORG_BLUETOOTH_CHARACTERISTIC_SCAN_INTERVAL_WINDOW) target = &peer->scan;
                if (characteristic.uuid16 == ORG_BLUETOOTH_CHARACTERISTIC_SCAN_REFRESH) target = &peer->refresh;
            }
            if (target) {
                if (target->value_handle) peer->bad_value = true;
                else *target = characteristic;
            }
            break;
        }
        case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT: {
            if (size < 12) { peer->bad_value = true; break; }
            uint16_t length = gatt_event_characteristic_value_query_result_get_value_length(packet);
            uint16_t attribute = gatt_event_characteristic_value_query_result_get_value_handle(packet);
            const uint8_t *value = gatt_event_characteristic_value_query_result_get_value(packet);
            if (length != size - 12u) { peer->bad_value = true; break; }
            if (peer->phase == HOGP_PNP_READ) {
                if (length != 7 || attribute != peer->pnp.value_handle || peer->pnp_valid ||
                    (value[0] != 1 && value[0] != 2)) { peer->bad_value = true; break; }
                peer->pnp_source = value[0];
                peer->vendor = little_endian_read_16(value, 1);
                peer->product = little_endian_read_16(value, 3);
                peer->version = little_endian_read_16(value, 5);
                peer->pnp_valid = true;
            } else if (peer->phase == HOGP_HID_INFO) {
                if (length != 4 || peer->info_count == HOGP_HID_SERVICES) {
                    peer->bad_value = true;
                    break;
                }
                peer->info_handles[peer->info_count] = attribute;
                peer->info_flags[peer->info_count++] = value[3];
            }
            break;
        }
        case GATT_EVENT_QUERY_COMPLETE:
            if (size < 9) {
                failure(peer, "truncated-completion", ERROR_CODE_UNSPECIFIED_ERROR);
                break;
            }
            completed(peer, gatt_event_query_complete_get_att_status(packet));
            break;
        default:
            break;
    }
}

void hub_hogp_stop(unsigned input) {
    if (input >= HUB_INPUT_COUNT) return;
    hogp_peer_t *peer = &peers[input];
    stop_listener(peer);
    peer->active = peer->pending = peer->refresh_pending = false;
}

void hub_hogp_init(void) {
    memset(peers, 0, sizeof(peers));
}

void hub_hogp_start(unsigned input, uint16_t handle) {
    if (input >= HUB_INPUT_COUNT) return;
    hub_hogp_stop(input);
    hogp_peer_t *peer = &peers[input];
    memset(peer, 0, sizeof(*peer));
    peer->active = true;
    peer->handle = handle;
    set_phase(peer, HOGP_DIS_SERVICE);
}

void hub_hogp_tick(void) {
    uint32_t now = btstack_run_loop_get_time_ms();
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        hogp_peer_t *peer = &peers[i];
        if (!peer->active) continue;
        if (peer->phase == HOGP_DONE && peer->refresh_pending && peer->scan_usable)
            set_phase(peer, HOGP_SCAN_WRITE);
        if (peer->phase == HOGP_DONE) continue;
        if ((int32_t)(now - peer->deadline) >= 0) {
            failure(peer, "timeout", ERROR_CODE_CONNECTION_TIMEOUT);
            /* An outstanding ATT transaction cannot be cancelled safely. */
            stop_listener(peer);
            peer->scan_usable = false;
            if (peer->phase != HOGP_DONE) finish(peer);
            continue;
        }
        if (peer->pending || !gatt_client_is_ready(peer->handle)) continue;
        if (!gap_encryption_key_size(peer->handle)) {
            failure(peer, "encryption-lost", ERROR_CODE_INSUFFICIENT_SECURITY);
            stop_listener(peer);
            peer->scan_usable = false;
            if (peer->phase != HOGP_DONE) finish(peer);
            continue;
        }
        uint8_t status;
        peer->pending = true;
        switch (peer->phase) {
            case HOGP_DIS_SERVICE:
            case HOGP_SCAN_SERVICE:
                status = gatt_client_discover_primary_services_by_uuid16(hogp_event, peer->handle,
                    peer->phase == HOGP_DIS_SERVICE ? ORG_BLUETOOTH_SERVICE_DEVICE_INFORMATION
                                                   : ORG_BLUETOOTH_SERVICE_SCAN_PARAMETERS);
                break;
            case HOGP_DIS_CHARACTERISTICS:
            case HOGP_SCAN_CHARACTERISTICS:
                status = gatt_client_discover_characteristics_for_service(hogp_event, peer->handle, &peer->service);
                break;
            case HOGP_PNP_READ:
                status = gatt_client_read_value_of_characteristic_using_value_handle(hogp_event, peer->handle, peer->pnp.value_handle);
                break;
            case HOGP_HID_INFO:
                status = gatt_client_read_value_of_characteristics_by_uuid16(hogp_event, peer->handle,
                    1, 0xffff, ORG_BLUETOOTH_CHARACTERISTIC_HID_INFORMATION);
                break;
            case HOGP_SCAN_CCCD:
                if (!peer->listening) {
                    gatt_client_listen_for_characteristic_value_updates(&peer->listener, hogp_event, peer->handle, &peer->refresh);
                    peer->listening = true;
                }
                status = gatt_client_write_client_characteristic_configuration(hogp_event, peer->handle, &peer->refresh,
                    GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
                break;
            case HOGP_SCAN_WRITE: {
                uint8_t value[4];
                little_endian_store_16(value, 0, HUB_INPUT_SCAN_INTERVAL);
                little_endian_store_16(value, 2, HUB_INPUT_SCAN_WINDOW);
                status = gatt_client_write_value_of_characteristic_without_response(peer->handle, peer->scan.value_handle, sizeof(value), value);
                if (!status) {
                    ++peer->writes;
                    peer->refresh_pending = false;
                    finish(peer);
                }
                break;
            }
            default:
                status = ERROR_CODE_COMMAND_DISALLOWED;
                break;
        }
        if (status) {
            peer->pending = false;
            if (status != GATT_CLIENT_IN_WRONG_STATE && status != GATT_CLIENT_BUSY
                && status != BTSTACK_ACL_BUFFERS_FULL)
                failure(peer, "local-API", status);
        }
    }
}

void hub_hogp_status(unsigned input) {
    if (input >= HUB_INPUT_COUNT) return;
    const hogp_peer_t *peer = &peers[input];
    printf("[hogp %u] handle=0x%04x active=%u phase=%s pending=%u degraded=%u errors=%" PRIu32 "\n",
           input + 1u, peer->handle, peer->active, phase_names[peer->phase], peer->pending, peer->degraded, peer->errors);
    if (peer->pnp_valid) {
        printf("[hogp %u] PnP source=%u vendor=0x%04x product=0x%04x version=0x%04x\n",
               input + 1u, peer->pnp_source, peer->vendor, peer->product, peer->version);
    } else printf("[hogp %u] PnP=not-validated\n", input + 1u);
    for (unsigned i = 0; i < peer->info_count; ++i) {
        printf("[hogp %u] HID-info=0x%04x flags=0x%02x remote-wake=%u normally-connectable=%u\n",
               input + 1u, peer->info_handles[i], peer->info_flags[i],
               !!(peer->info_flags[i] & 1u), !!(peer->info_flags[i] & 2u));
    }
    printf("[hogp %u] Scan-Parameters=%s usable=%u refresh-subscribed=%u refreshes=%" PRIu32
           " writes-submitted=%" PRIu32 " active-initiation-scan=%u/%u(0.625ms); reconnect backoff unchanged\n",
           input + 1u, !peer->scan_checked ? "unknown" : peer->scan_present ? "present" : "absent",
           peer->scan_usable, peer->scan_subscribed, peer->refreshes, peer->writes,
           HUB_INPUT_SCAN_INTERVAL, HUB_INPUT_SCAN_WINDOW);
}
