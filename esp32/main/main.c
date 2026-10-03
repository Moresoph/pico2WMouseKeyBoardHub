#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_hidh.h"
#include "esp_console.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "hub_link.h"
#include "hub_remote_hid.h"
#include "input_policy.h"
#include "hub_web.h"
#include "hub_classic_security.h"
#include "reconnect_policy.h"
#include "hub_ui.h"
#include "auto_input_policy.h"

static const char *TAG = "dual-bt";
_Static_assert(ESP_HID_TRANSPORT_BT==HUB_RECORD_CLASSIC && ESP_HID_TRANSPORT_BLE==HUB_RECORD_BLE,
               "Input registry transport values must match ESP-IDF");
static const char CLASSIC_FIXED_PIN[] = "123456";
enum {
    LINK_UART_TX_PIN = GPIO_NUM_13,
    LINK_UART_RX_PIN = GPIO_NUM_14,
    STATUS_LED_PIN = GPIO_NUM_2
};
enum { AUTH_NONE, AUTH_CONFIRM, AUTH_PASSKEY, AUTH_PIN };
enum { INPUT_PAIRING_WINDOW_MS = 120000 };
enum { BLE_MOUSE_INTERVAL_UNITS = 6, BLE_MOUSE_INTERVAL_MAX_ATTEMPTS = 3 };
typedef enum {
    LED_IDLE,
    LED_RECONNECTING,
    LED_PAIRING,
    LED_ENTER_FIXED_PIN,
    LED_READY,
    LED_FAULT
} status_led_mode_t;
typedef struct {
    bool selected, paused, pending, mapped, secure, pin16, pin_replied, allow_open, interval_pending;
    bool classic_incoming;
    bool classic_acl_connected, classic_acl_encrypted;
    bool encryption_pending;
    uint32_t acl_generation;
    uint32_t encryption_token;
    uint16_t classic_handle;
    bool open_running, recovering, profile_requested;
    uint8_t reconnect_failures;
    uint8_t profile_failures;
    uint32_t recovery_at;
    bool bonded;
    uint16_t interval;
    char name[256];
    uint8_t address[6], transport, address_type, auth, seen_roles;
    uint8_t interval_attempts;
    uint32_t auth_number;
    uint32_t pair_until, retry_at, opened_at, reports, invalid, incoming_connections;
    esp_hidh_dev_t *dev;
    hub_remote_hid_t hid;
    hub_link_input_t input;
} peer_t;
static peer_t peers[HUB_LINK_SLOTS];
static SemaphoreHandle_t mutex;
static QueueHandle_t snapshots, uart_events, ble_opens;
static nvs_handle_t store;
static uint32_t nonce, session, request_at, sent_at, sent_count, overflow_count, encryption_token;
static uint16_t sequence;
static bool classic_ready, scanning, scan_ble_pending;
enum { SCAN_NONE, SCAN_CLASSIC, SCAN_BLE };
static uint8_t requested_scan, active_scan;
static uint32_t scan_requested_at, scan_started_at;
static hub_link_parser_t parser;
static hub_link_output_cache_t pico_output;
typedef struct {
    uint8_t address[6], status, flags;
    uint32_t generation;
    bool connected, encrypted, security_failure;
} classic_encryption_event_t;
static QueueHandle_t encryption_events;
static hub_input_records_t saved_inputs;
static bool records_fault;
static bool record_names_dirty;
static bool auto_enrollment, auto_scan_complete, auto_committed;
static uint8_t auto_stage;
static uint32_t auto_retry_at;
static hub_web_candidate_t candidates[HUB_WEB_CANDIDATES];
static uint8_t candidate_count;
static uint32_t management_until, led_ack_until, ui_sent_at, pc_request_at;
static uint16_t ui_sequence;
static uint16_t pc_sequence;
static char management_token[17], operation[192];
static QueueHandle_t management_commands;
static hub_link_frame_t pc_request;
static uint8_t pc_attempts;
typedef struct {
    uint8_t phase, kind, role, record;
    bool abort, was_bonded;
    hub_saved_input_t target;
} input_trial_t;
static input_trial_t trial;
static uint32_t now_ms(void);
static esp_err_t save_target(unsigned i,const uint8_t *address,uint8_t transport,uint8_t type,bool allow_open);
static void manage_tick(void);
static void update_saved_name(const uint8_t address[6], const char *name);
static bool management_open(void){return management_until && (int32_t)(now_ms()-management_until)<0;}

void hub_esp_classic_encryption_result(const uint8_t address[6], uint32_t generation,
                                      bool connected, bool encrypted, bool security_failure,
                                      uint8_t status, uint8_t flags) {
    classic_encryption_event_t event = {.generation = generation, .encrypted = encrypted,
                                        .connected = connected, .security_failure = security_failure,
                                        .status = status, .flags = flags};
    memcpy(event.address, address, 6);
    configASSERT(xQueueSend(encryption_events, &event, 0) == pdTRUE);
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static bool due(uint32_t now, uint32_t at) { return (int32_t)(now - at) >= 0; }
static void lock(void) { xSemaphoreTake(mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(mutex); }
static uint32_t random_nonzero(void) {
    uint32_t n;
    do { n = esp_random(); } while (!n);
    return n;
}
static void check_result(esp_err_t error, const char *operation) {
    if (error != ESP_OK) ESP_LOGE(TAG, "%s failed: %s", operation, esp_err_to_name(error));
}
static peer_t *peer_for(const uint8_t *address, unsigned transport) {
    if (!address) return NULL;
    for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i)
        if (peers[i].selected && peers[i].transport == transport &&
            memcmp(peers[i].address, address, 6) == 0) return &peers[i];
    return NULL;
}
bool hub_esp_classic_recovery_current(const uint8_t address[6], uint16_t handle, uint32_t generation) {
    if (xSemaphoreTake(mutex, 0) != pdTRUE) return false;
    peer_t *p = peer_for(address, ESP_HID_TRANSPORT_BT);
    bool current = p && p->recovering && p->classic_acl_connected &&
        p->classic_handle == handle && p->acl_generation == generation;
    unlock();
    return current;
}
static bool pairing_window_open(const peer_t *p) {
    return p && input_pairing_window_open(p->selected, p->paused, now_ms(), p->pair_until);
}
static bool pairing_allowed(const peer_t *p) {
    return pairing_window_open(p) && p->pending;
}
static bool classic_bonded(const uint8_t address[6]) {
    int count = esp_bt_gap_get_bond_device_num();
    if (count <= 0) return false;
    esp_bd_addr_t *bonds = calloc((size_t)count, sizeof(*bonds));
    if (!bonds) {
        ESP_LOGE(TAG, "cannot read Classic bonds: out of memory");
        return false;
    }
    esp_err_t error = esp_bt_gap_get_bond_device_list(&count, bonds);
    check_result(error, "read Classic bonds");
    bool found = false;
    if (error == ESP_OK)
        for (int i = 0; i < count; ++i)
            if (!memcmp(bonds[i], address, 6)) found = true;
    free(bonds);
    return found;
}
static bool classic_admitted(const peer_t *p) {
    if (!p || !p->selected || p->paused) return false;
    return input_classic_admitted(p->selected, p->paused, classic_bonded(p->address),
                                  pairing_window_open(p), p->allow_open);
}
static void begin_classic_incoming(peer_t *p) {
    p->classic_incoming = true;
    p->pending = true;
    p->opened_at = now_ms();
    ++p->incoming_connections;
    ESP_LOGI(TAG, "input %u accepted saved-target Classic incoming connection; awaiting encrypted HID",
             (unsigned)(p - peers) + 1);
}
static bool ble_bonded(const uint8_t address[6]) {
    int count = esp_ble_get_bond_device_num();
    if (count <= 0) return false;
    esp_ble_bond_dev_t *bonds = calloc((size_t)count, sizeof(*bonds));
    if (!bonds) {
        ESP_LOGE(TAG, "cannot read BLE bonds: out of memory");
        return false;
    }
    esp_err_t error = esp_ble_get_bond_device_list(&count, bonds);
    check_result(error, "read BLE bonds");
    bool found = false;
    if (error == ESP_OK)
        for (int i = 0; i < count; ++i)
            if (!memcmp(bonds[i].bd_addr, address, 6)) found = true;
    free(bonds);
    return found;
}
bool hub_esp_allow_open_input(const unsigned char address[6]) {
    lock();
    peer_t *p = peer_for(address, ESP_HID_TRANSPORT_BT);
    bool allowed = p && !p->paused && p->pending && p->allow_open;
    unlock();
    return allowed;
}
static bool forwarding_allowed(const peer_t *p) {
    return p->secure || (p->transport == ESP_HID_TRANSPORT_BT && p->allow_open);
}
static void request_mouse_interval(peer_t *p) {
    if (!p || p->paused || !p->mapped || !p->secure ||
        p->transport != ESP_HID_TRANSPORT_BLE || !(p->hid.roles & HUB_LINK_MOUSE) ||
        p->interval_pending) return;
    unsigned slot = (unsigned)(p - peers) + 1;
    esp_gap_conn_params_t current;
    esp_err_t error = esp_ble_get_current_conn_params(p->address, &current);
    if (error != ESP_OK) {
        check_result(error, "read BLE mouse connection parameters");
        return;
    }
    p->interval = current.interval;
    if (current.interval == BLE_MOUSE_INTERVAL_UNITS) return;
    if (p->interval_attempts >= BLE_MOUSE_INTERVAL_MAX_ATTEMPTS) {
        ESP_LOGW(TAG, "input %u 7.5ms request limit reached; retaining negotiated BLE parameters", slot);
        return;
    }
    esp_ble_conn_update_params_t update = {
        .min_int = BLE_MOUSE_INTERVAL_UNITS,
        .max_int = BLE_MOUSE_INTERVAL_UNITS,
        .latency = current.latency,
        .timeout = current.timeout
    };
    memcpy(update.bda, p->address, sizeof(update.bda));
    ++p->interval_attempts;
    error = esp_ble_gap_update_conn_params(&update);
    check_result(error, "request 7.5ms BLE mouse interval");
    p->interval_pending = error == ESP_OK;
    if (p->interval_pending)
        ESP_LOGI(TAG, "input %u requested BLE interval=7.500ms; preserving latency=%u timeout=%ums; awaiting negotiation",
                 slot, current.latency, current.timeout * 10u);
}
static void snapshot_locked(hub_link_input_t out[HUB_LINK_SLOTS]) {
    for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i) {
        peer_t *p = &peers[i];
        out[i] = p->input;
        if (!p->mapped || !forwarding_allowed(p) || p->paused ||
            (trial.phase && trial.role == i + 1)) {
            memset(&out[i], 0, sizeof(out[i]));
            out[i].epoch = p->input.epoch;
        } else out[i].roles = p->seen_roles;
    }
}
static void queue_state(void) {
    if (!nonce || (uint32_t)(now_ms() - request_at) >= HUB_LINK_TIMEOUT_MS) return;
    hub_link_input_t snapshot[HUB_LINK_SLOTS];
    snapshot_locked(snapshot);
    if (xQueueSend(snapshots, snapshot, 0) != pdTRUE) {
        ++overflow_count;
        session = random_nonzero();
        xQueueReset(snapshots);
        for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i)
            snapshot[i].dx = snapshot[i].dy = snapshot[i].wheel = 0;
        configASSERT(xQueueSend(snapshots, snapshot, 0) == pdTRUE);
        ESP_LOGE(TAG, "input queue overflow; new session forces Pico release, count=%" PRIu32, overflow_count);
    }
}
static void release_peer(peer_t *p) {
    p->mapped = p->secure = false;
    p->seen_roles = p->auth = 0;
    p->pin_replied = false;
    p->interval_pending = false;
    p->interval_attempts = 0;
    p->interval = 0;
    p->encryption_pending = false;
    uint8_t epoch = p->input.epoch + 1u;
    memset(&p->input, 0, sizeof(p->input));
    p->input.epoch = epoch;
    queue_state();
}
static void retry_peer(peer_t *p) {
    p->pending = p->recovering = p->profile_requested = false;
    if (p->reconnect_failures < 5) ++p->reconnect_failures;
    p->retry_at = now_ms() + input_reconnect_backoff(p->reconnect_failures);
}
static void recover_peer(peer_t *p, const char *reason) {
    if (!p->recovering) {
        p->recovering = p->pending = true;
        p->recovery_at = now_ms();
        release_peer(p);
        ESP_LOGW(TAG, "input %u recovery: %s; bond/pairing policy unchanged",
                 (unsigned)(p - peers) + 1, reason);
    }
    if (p->classic_acl_connected) {
        check_result(hub_esp_classic_close_acl(p->address, p->classic_handle, p->acl_generation),
                     "close stalled Classic ACL without deleting bond");
    } else if (p->dev) {
        check_result(esp_hidh_dev_close(p->dev), "close recovering input");
    } else if (!p->open_running) retry_peer(p);
}
static void reject_auth(peer_t *p) {
    if (p->pin_replied) {
        /* The positive PIN reply has already been sent; only clear local state. */
    } else if (p->auth == AUTH_CONFIRM) {
        check_result(p->transport == ESP_HID_TRANSPORT_BT
            ? esp_bt_gap_ssp_confirm_reply(p->address, false)
            : esp_ble_confirm_reply(p->address, false), "decline confirmation");
    } else if (p->auth == AUTH_PASSKEY) {
        check_result(p->transport == ESP_HID_TRANSPORT_BT
            ? esp_bt_gap_ssp_passkey_reply(p->address, false, 0)
            : esp_ble_passkey_reply(p->address, false, 0), "decline passkey");
    } else if (p->auth == AUTH_PIN) {
        esp_bt_pin_code_t pin = {0};
        check_result(esp_bt_gap_pin_reply(p->address, false, 0, pin), "decline PIN");
    }
    p->auth = AUTH_NONE;
    p->pin_replied = false;
}
static void auth_request(peer_t *p, unsigned kind, uint32_t number) {
    if (!p) return;
    p->auth = (uint8_t)kind;
    p->auth_number = number;
    p->pin_replied = false;
    if (!pairing_allowed(p)) {
        reject_auth(p);
        ESP_LOGW(TAG, "pairing outside explicit connect window rejected");
        return;
    }
    unsigned slot = (unsigned)(p - peers) + 1;
    if (kind == AUTH_CONFIRM)
        ESP_LOGI(TAG, "input %u compare %06" PRIu32 "; then: confirm %u yes|no", slot, number, slot);
    else if (kind == AUTH_PIN) {
        if (p->pin16) {
            reject_auth(p);
            p->paused = true;
            release_peer(p);
            if (p->dev) check_result(esp_hidh_dev_close(p->dev), "close incompatible PIN input");
            ESP_LOGE(TAG, "input %u requires a 16-character legacy PIN; fixed PIN %s is incompatible; paused",
                     slot, CLASSIC_FIXED_PIN);
            return;
        }
        esp_bt_pin_code_t pin = {0};
        memcpy(pin, CLASSIC_FIXED_PIN, sizeof(CLASSIC_FIXED_PIN) - 1);
        esp_err_t error = esp_bt_gap_pin_reply(
            p->address, true, sizeof(CLASSIC_FIXED_PIN) - 1, pin);
        check_result(error, "fixed legacy PIN reply");
        if (error != ESP_OK) {
            p->auth = AUTH_NONE;
            p->paused = true;
            release_peer(p);
            if (p->dev) check_result(esp_hidh_dev_close(p->dev), "close after PIN reply failure");
            return;
        }
        p->pin_replied = true;
        ESP_LOGI(TAG, "input %u: LED fast blink; type fixed PIN %s then Enter ON THE BLUETOOTH KEYBOARD",
                 slot, CLASSIC_FIXED_PIN);
    } else {
        ESP_LOGI(TAG, "input %u needs passkey; use passkey %u <six digits>", slot, slot);
    }
}

static status_led_mode_t status_led_mode_locked(uint32_t now) {
    bool selected = false, all_ready = true, pairing = false, fault = false;
    for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i) {
        const peer_t *p = &peers[i];
        if (!p->selected) continue;
        selected = true;
        if (p->auth == AUTH_PIN && p->pin_replied) return LED_ENTER_FIXED_PIN;
        pairing |= !p->paused && p->pair_until && !due(now, p->pair_until);
        fault |= p->paused;
        all_ready &= p->dev && p->mapped && forwarding_allowed(p);
    }
    if (pairing || management_open()) return LED_PAIRING;
    if (!nonce && now > 3000) fault = true;
    if (fault || records_fault) return LED_FAULT;
    if (selected && all_ready) return LED_READY;
    return selected ? LED_RECONNECTING : LED_IDLE;
}

static bool status_led_level(status_led_mode_t mode, uint32_t now) {
    switch (mode) {
        case LED_ENTER_FIXED_PIN:
            return (now / 100u) % 2u == 0;
        case LED_PAIRING:
            return (now / 500u) % 2u == 0;
        case LED_READY:
            return false;
        case LED_FAULT:
            return hub_ui_led(HUB_LED_FAULT, now);
        case LED_RECONNECTING:
            return now % 2000u < 100u;
        default:
            return false;
    }
}

static const char *status_led_name(status_led_mode_t mode) {
    static const char *const names[] = {
        "idle", "reconnecting", "pairing", "enter-fixed-pin", "ready", "fault"
    };
    return names[mode];
}
static void remember_candidate(const uint8_t address[6],uint8_t transport,uint8_t type,
                                const uint8_t *name,unsigned length,bool hid,uint8_t roles) {
    char discovered_name[HUB_INPUT_NAME_BYTES] = {0};
    if (name && length) {
        char source[256];
        if (length >= sizeof(source)) length = sizeof(source) - 1;
        memcpy(source, name, length); source[length] = 0;
        hub_input_name_copy(discovered_name, source);
        update_saved_name(address, discovered_name);
    }
    if(!management_open() || trial.phase)return;
    if (hub_input_is_hub_name(discovered_name)) {
        for(unsigned i=0;i<candidate_count;++i)
            if(candidates[i].transport==transport && !memcmp(candidates[i].address,address,6)){
                memmove(&candidates[i],&candidates[i+1],(candidate_count-i-1)*sizeof(candidates[0]));
                --candidate_count;break;
            }
        return;
    }
    if(type>1 || (!hid && (!name || !length)))return;
    unsigned i=0;
    for(;i<candidate_count;++i)if(candidates[i].transport==transport &&
        !memcmp(candidates[i].address,address,6))break;
    if(i==candidate_count){
        if(candidate_count==HUB_WEB_CANDIDATES){
            if(!hid)return;
            for(i=0;i<candidate_count;++i)if(!candidates[i].hid)break;
            if(i==candidate_count)return;
        }else ++candidate_count;
        memset(&candidates[i],0,sizeof(candidates[i]));
    }
    hub_web_candidate_t *c=&candidates[i];c->transport=transport;c->type=type;c->hid|=hid;c->roles|=roles;
    memcpy(c->address,address,6);
    if(discovered_name[0])memcpy(c->name,discovered_name,sizeof(c->name));
}

static void hid_event(void *arg, esp_event_base_t base, int32_t event, void *data) {
    (void)arg; (void)base;
    esp_hidh_event_data_t *e = data;
    if (event == ESP_HIDH_START_EVENT) {
        lock();
        classic_ready = e->start.status == ESP_OK;
        unlock();
        check_result(e->start.status, "Classic HID start");
        return;
    }
    esp_hidh_dev_t *dev = event == ESP_HIDH_OPEN_EVENT ? e->open.dev :
        event == ESP_HIDH_CLOSE_EVENT ? e->close.dev :
        event == ESP_HIDH_INPUT_EVENT ? e->input.dev : NULL;
    if (!dev) return;
    const uint8_t *address = esp_hidh_dev_bda_get(dev);
    unsigned transport = esp_hidh_dev_transport_get(dev);
    lock();
    peer_t *p = peer_for(address, transport);
    if (p && p->dev && p->dev != dev &&
        (event == ESP_HIDH_CLOSE_EVENT || (event == ESP_HIDH_OPEN_EVENT && e->open.status != ESP_OK))) {
        ESP_LOGW(TAG, "ignoring callback for superseded HID object");
        unlock();
        return;
    }
    if (!p || p->paused || p->recovering) {
        if (event == ESP_HIDH_OPEN_EVENT && e->open.status == ESP_OK) {
            if (p) p->dev = dev;
            ESP_LOGW(TAG, "reject HID open from unselected/paused input " ESP_BD_ADDR_STR,
                     ESP_BD_ADDR_HEX(address));
            check_result(esp_hidh_dev_close(dev), "close unselected input");
        }
        if (p && (event == ESP_HIDH_CLOSE_EVENT ||
                  (event == ESP_HIDH_OPEN_EVENT && e->open.status != ESP_OK))) {
            p->dev = NULL;
            release_peer(p);
            if (!p->classic_acl_connected) {
                p->pending = false;
                if (!p->open_running) retry_peer(p);
            }
        }
        unlock();
        return;
    }
    unsigned slot = (unsigned)(p - peers) + 1;
    if (event == ESP_HIDH_OPEN_EVENT) {
        if (e->open.status != ESP_OK) {
            if (p->dev && p->dev != dev) {
                ESP_LOGW(TAG, "input %u ignored obsolete OPEN failure", slot);
                unlock();
                return;
            }
            p->dev = NULL;
            release_peer(p);
            if (p->classic_acl_connected) {
                if (++p->profile_failures >= 3) recover_peer(p, "repeated HID setup failures on live ACL");
                else {
                    /* A failed outgoing page may race the keyboard's successful incoming ACL. */
                    p->pending = true;
                    p->profile_requested = false;
                    p->opened_at = now_ms();
                    ESP_LOGW(TAG, "input %u OPEN failed but ACL is live; await/reattach HID instead of discarding incoming link", slot);
                }
            }
            else {
                p->pending = false;
                if (!p->open_running) retry_peer(p);
            }
            ESP_LOGE(TAG, "input %u open failed: %s", slot, esp_err_to_name(e->open.status));
        } else {
            if (transport == ESP_HID_TRANSPORT_BT && !classic_admitted(p)) {
                p->dev = dev;
                p->pending = true;
                p->opened_at = now_ms();
                p->paused = true;
                reject_auth(p);
                release_peer(p);
                check_result(esp_hidh_dev_close(dev), "close Classic input without bond/consent");
                ESP_LOGW(TAG, "input %u Classic admission rejected: no bond or explicit pairing window; paused", slot);
                unlock();
                return;
            }
            if (transport == ESP_HID_TRANSPORT_BT && !p->pending && !p->dev)
                begin_classic_incoming(p);
            p->dev = dev;
            p->pending = true;
            p->opened_at = now_ms();
            if (transport == ESP_HID_TRANSPORT_BT)
                p->secure = p->classic_acl_encrypted;
            const char *name = esp_hidh_dev_name_get(dev);
            if (name && name[0]) {
                snprintf(p->name, sizeof(p->name), "%s", name);
                update_saved_name(p->address, name);
            }
            size_t count = 0;
            esp_hid_raw_report_map_t *maps = NULL;
            memset(&p->hid, 0, sizeof(p->hid));
            bool valid = esp_hidh_dev_report_maps_get(dev, &count, &maps) == ESP_OK &&
                count > 0 && count <= HUB_REMOTE_MAPS;
            for (size_t i = 0; valid && i < count; ++i)
                valid = hub_remote_hid_map(&p->hid, i, maps[i].data, maps[i].len);
            p->mapped = valid && p->hid.roles;
            if (!p->mapped) {
                p->paused = true;
                release_peer(p);
                check_result(esp_hidh_dev_close(dev), "close unsupported HID");
                ESP_LOGE(TAG, "input %u unsupported HID descriptor; paused, no boot-layout guessing", slot);
            } else {
                ESP_LOGI(TAG, "input %u HID open roles=%u encrypted=%u; waiting for real input", slot, p->hid.roles, p->secure);
                if (transport == ESP_HID_TRANSPORT_BLE && !p->secure)
                    check_result(esp_ble_set_encryption(p->address, ESP_BLE_SEC_ENCRYPT), "BLE encryption");
                request_mouse_interval(p);
                queue_state();
            }
        }
    } else if (event == ESP_HIDH_CLOSE_EVENT) {
        if (p->dev && p->dev != dev) { unlock(); return; }
        p->dev = NULL;
        release_peer(p);
        p->pending = p->classic_acl_connected;
        if (!p->classic_acl_connected && !p->open_running) retry_peer(p);
        ESP_LOGI(TAG, "input %u closed; reason=%d", slot, e->close.reason);
        /* Pinned IDF 5.5.1 postprocess handler frees the device after this callback. */
    } else if (event == ESP_HIDH_INPUT_EVENT && p->dev == dev && p->mapped && forwarding_allowed(p)) {
        if (!hub_remote_hid_report(&p->hid, e->input.map_index, e->input.report_id,
                                  e->input.data, e->input.length, &p->input)) {
            ++p->invalid;
            p->paused = true;
            release_peer(p);
            check_result(esp_hidh_dev_close(dev), "close malformed HID");
            ESP_LOGE(TAG, "input %u invalid report map=%u id=%u len=%u; paused", slot,
                     e->input.map_index, e->input.report_id, e->input.length);
        } else {
            for (unsigned i = 0; i < p->hid.count; ++i)
                if (p->hid.reports[i].map == e->input.map_index &&
                    p->hid.reports[i].id == e->input.report_id)
                    p->seen_roles |= p->hid.reports[i].roles;
            ++p->reports;
            if (p->reports == 1) ESP_LOGI(TAG, "input %u first decoded HID report", slot);
            queue_state();
            p->input.dx = p->input.dy = p->input.wheel = 0;
        }
    }
    unlock();
}

static void classic_gap(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *e) {
    lock();
    peer_t *p = NULL;
    switch (event) {
        case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
            p = peer_for(e->acl_conn_cmpl_stat.bda, ESP_HID_TRANSPORT_BT);
            ESP_LOGI(TAG, "Classic ACL " ESP_BD_ADDR_STR " status=%u handle=0x%04x",
                     ESP_BD_ADDR_HEX(e->acl_conn_cmpl_stat.bda),
                     e->acl_conn_cmpl_stat.stat, e->acl_conn_cmpl_stat.handle);
            if (p && e->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS) {
                if (!p->classic_acl_connected) {
                    ++p->acl_generation;
                    p->profile_failures = 0;
                }
                p->classic_acl_connected = true;
                p->classic_handle = e->acl_conn_cmpl_stat.handle;
                if (!p->mapped) {
                    p->pending = true;
                    p->opened_at = now_ms();
                }
            }
            if (e->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS && (!p || !p->profile_requested)) {
                if (classic_admitted(p) && !p->recovering) begin_classic_incoming(p);
                else ESP_LOGW(TAG, "unsolicited Classic ACL is not authorized for HID; pairing requests will be rejected");
            }
            break;
        case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
            p = peer_for(e->acl_disconn_cmpl_stat.bda, ESP_HID_TRANSPORT_BT);
            if (p) {
                if (!p->classic_acl_connected || p->classic_handle != e->acl_disconn_cmpl_stat.handle) break;
                ESP_LOGI(TAG, "input %u Classic ACL disconnected reason=%u",
                         (unsigned)(p - peers) + 1, e->acl_disconn_cmpl_stat.reason);
                p->classic_acl_connected = p->classic_acl_encrypted = false;
                ++p->acl_generation;
                if (!p->dev) {
                    if (!p->open_running) retry_peer(p);
                }
                release_peer(p);
                p->classic_incoming = false;
            }
            break;
        case ESP_BT_GAP_DISC_RES_EVT:
            ESP_LOGI(TAG, "scan classic " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(e->disc_res.bda));
            {
                const uint8_t *name=NULL;unsigned length=0;bool hid=false;uint8_t roles=0;
                for(int i=0;i<e->disc_res.num_prop;++i){
                    esp_bt_gap_dev_prop_t *prop=&e->disc_res.prop[i];
                    if(prop->type==ESP_BT_GAP_DEV_PROP_BDNAME){name=prop->val;length=prop->len;}
                    else if(prop->type==ESP_BT_GAP_DEV_PROP_COD && prop->len>=4){
                        uint32_t cod;memcpy(&cod,prop->val,4);hid=((cod>>8)&31u)==5;
                        if(hid)roles=((cod&0x40u)?HUB_LINK_KEYBOARD:0)|((cod&0x80u)?HUB_LINK_MOUSE:0);
                    } else if(prop->type==ESP_BT_GAP_DEV_PROP_EIR && !name){
                        uint8_t n=0;name=esp_bt_gap_resolve_eir_data(prop->val,ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME,&n);length=n;
                    }
                }
                remember_candidate(e->disc_res.bda,ESP_HID_TRANSPORT_BT,0,name,length,hid,roles);
            }
            break;
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            scanning = e->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED;
            if(!scanning && active_scan==SCAN_CLASSIC){
                active_scan=SCAN_NONE;
                auto_scan_complete=true;
                if(management_open())snprintf(operation,sizeof(operation),auto_enrollment?
                    "扫描完成：找到 %u 个候选，正在检查唯一新键鼠":
                    "扫描完成：找到 %u 个候选，请选择“作为鼠标录入”",candidate_count);
            }
            break;
        case ESP_BT_GAP_CFM_REQ_EVT:
            p = peer_for(e->cfm_req.bda, ESP_HID_TRANSPORT_BT);
            if (p) auth_request(p, AUTH_CONFIRM, e->cfm_req.num_val);
            else check_result(esp_bt_gap_ssp_confirm_reply(e->cfm_req.bda, false), "reject unknown peer");
            break;
        case ESP_BT_GAP_KEY_REQ_EVT:
            p = peer_for(e->key_req.bda, ESP_HID_TRANSPORT_BT);
            if (p) auth_request(p, AUTH_PASSKEY, 0);
            else check_result(esp_bt_gap_ssp_passkey_reply(e->key_req.bda, false, 0), "reject unknown passkey");
            break;
        case ESP_BT_GAP_PIN_REQ_EVT: {
            p = peer_for(e->pin_req.bda, ESP_HID_TRANSPORT_BT);
            if (p) { p->pin16 = e->pin_req.min_16_digit; auth_request(p, AUTH_PIN, 0); }
            else { esp_bt_pin_code_t pin = {0}; check_result(esp_bt_gap_pin_reply(e->pin_req.bda, false, 0, pin), "reject unknown PIN"); }
            break;
        }
        case ESP_BT_GAP_KEY_NOTIF_EVT:
            p = peer_for(e->key_notif.bda, ESP_HID_TRANSPORT_BT);
            if (pairing_allowed(p))
                ESP_LOGI(TAG, "type %06" PRIu32 " then Enter ON THE BLUETOOTH KEYBOARD", e->key_notif.passkey);
            break;
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            p = peer_for(e->auth_cmpl.bda, ESP_HID_TRANSPORT_BT);
            if (p) {
                p->auth = AUTH_NONE;
                p->pin_replied = false;
                p->bonded = classic_bonded(p->address);
                ESP_LOGI(TAG, "input %u Classic auth status=%u", (unsigned)(p - peers) + 1, e->auth_cmpl.stat);
                if (e->auth_cmpl.stat != ESP_BT_STATUS_SUCCESS) {
                    p->paused = e->auth_cmpl.stat == ESP_BT_STATUS_AUTH_FAILURE ||
                        e->auth_cmpl.stat == ESP_BT_STATUS_AUTH_REJECTED ||
                        e->auth_cmpl.stat == ESP_BT_STATUS_HCI_AUTH_FAILURE ||
                        e->auth_cmpl.stat == ESP_BT_STATUS_HCI_KEY_MISSING ||
                        e->auth_cmpl.stat == ESP_BT_STATUS_HCI_HOST_REJECT_SECURITY;
                    recover_peer(p, p->paused ? "authentication/key failure; explicit action required" : "temporary authentication transport failure");
                }
            }
            break;
        case ESP_BT_GAP_ENC_CHG_EVT:
            p = peer_for(e->enc_chg.bda, ESP_HID_TRANSPORT_BT);
            if (p && p->classic_acl_connected) {
                bool was_secure = p->secure;
                p->secure = e->enc_chg.enc_mode != ESP_BT_ENC_MODE_OFF;
                p->classic_acl_encrypted = p->secure;
                ESP_LOGI(TAG, "input %u GAP encryption mode=%u generation=%" PRIu32,
                         (unsigned)(p - peers) + 1, e->enc_chg.enc_mode, p->acl_generation);
                if (was_secure && !forwarding_allowed(p)) recover_peer(p, "encryption disabled");
                queue_state();
            }
            break;
        default: break;
    }
    unlock();
}

static void ble_gap(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *e) {
    lock();
    peer_t *p = NULL;
    switch (event) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            if (scan_ble_pending) {
                scan_ble_pending = false;
                esp_err_t error = e->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS &&
                    (!active_scan || management_open())
                    ? esp_ble_gap_start_scanning(6) : ESP_FAIL;
                if (error != ESP_OK) {
                    scanning = false;active_scan=SCAN_NONE;
                    snprintf(operation,sizeof(operation),"BLE 扫描未启动，请重新授权后重试");
                }
                check_result(error, "BLE scan");
            }
            break;
        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            if (e->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
                scanning = false;
                if(active_scan==SCAN_BLE){
                    active_scan=SCAN_NONE;
                    auto_scan_complete=true;
                    if(management_open())snprintf(operation,sizeof(operation),auto_enrollment?
                        "扫描完成：找到 %u 个候选，正在检查唯一新键鼠":
                        "扫描完成：找到 %u 个候选，请选择“作为鼠标录入”",candidate_count);
                }
            }
            else if (e->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                uint16_t data_len = e->scan_rst.adv_data_len + e->scan_rst.scan_rsp_len;
                uint8_t value_len = 0;
                uint8_t *name = esp_ble_resolve_adv_data_by_type(
                    e->scan_rst.ble_adv, data_len, ESP_BLE_AD_TYPE_NAME_CMPL, &value_len);
                if (!name) name = esp_ble_resolve_adv_data_by_type(
                    e->scan_rst.ble_adv, data_len, ESP_BLE_AD_TYPE_NAME_SHORT, &value_len);
                uint8_t name_len = value_len;
                uint8_t *services = esp_ble_resolve_adv_data_by_type(
                    e->scan_rst.ble_adv, data_len, ESP_BLE_AD_TYPE_16SRV_CMPL, &value_len);
                if (!services) services = esp_ble_resolve_adv_data_by_type(
                    e->scan_rst.ble_adv, data_len, ESP_BLE_AD_TYPE_16SRV_PART, &value_len);
                bool hid = false;
                for (unsigned i = 0; services && i + 1 < value_len; i += 2)
                    hid |= services[i] == 0x12 && services[i + 1] == 0x18;
                uint8_t appearance_len = 0;
                uint8_t *appearance_data = esp_ble_resolve_adv_data_by_type(
                    e->scan_rst.ble_adv, data_len, ESP_BLE_AD_TYPE_APPEARANCE, &appearance_len);
                uint16_t appearance = appearance_data && appearance_len >= 2
                    ? (uint16_t)(appearance_data[0] | appearance_data[1] << 8) : 0;
                remember_candidate(e->scan_rst.bda,ESP_HID_TRANSPORT_BLE,e->scan_rst.ble_addr_type,
                                   name,name_len,hid || (appearance&0xffc0u)==0x03c0u,
                                   appearance==0x03c1u?HUB_LINK_KEYBOARD:appearance==0x03c2u?HUB_LINK_MOUSE:0);
                ESP_LOGI(TAG, "scan ble %s " ESP_BD_ADDR_STR
                         " rssi=%d hid=%u appearance=0x%04x name=%.*s",
                         e->scan_rst.ble_addr_type == BLE_ADDR_TYPE_PUBLIC ? "public" : "random",
                         ESP_BD_ADDR_HEX(e->scan_rst.bda), e->scan_rst.rssi, hid, appearance,
                         name ? name_len : 0, name ? (char *)name : "");
            }
            break;
        case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
            if (e->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                scanning = false;
                active_scan=SCAN_NONE;
                snprintf(operation,sizeof(operation),"BLE 扫描启动失败，旧记录未改变");
                ESP_LOGE(TAG, "BLE scan start status=%u", e->scan_start_cmpl.status);
            }
            break;
        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
            if(e->scan_stop_cmpl.status==ESP_BT_STATUS_SUCCESS){
                scanning=false;active_scan=SCAN_NONE;
            }else ESP_LOGE(TAG,"BLE scan stop status=%u",e->scan_stop_cmpl.status);
            break;
        case ESP_GAP_BLE_SEC_REQ_EVT:
            p = peer_for(e->ble_security.ble_req.bd_addr, ESP_HID_TRANSPORT_BLE);
            check_result(esp_ble_gap_security_rsp(e->ble_security.ble_req.bd_addr,
                pairing_allowed(p) || (p && !p->paused && p->pending && ble_bonded(p->address))),
                "BLE security reply");
            break;
        case ESP_GAP_BLE_NC_REQ_EVT:
            p = peer_for(e->ble_security.key_notif.bd_addr, ESP_HID_TRANSPORT_BLE);
            if (p) auth_request(p, AUTH_CONFIRM, e->ble_security.key_notif.passkey);
            else check_result(esp_ble_confirm_reply(e->ble_security.key_notif.bd_addr, false), "reject unknown BLE peer");
            break;
        case ESP_GAP_BLE_PASSKEY_REQ_EVT:
            p = peer_for(e->ble_security.ble_req.bd_addr, ESP_HID_TRANSPORT_BLE);
            if (p) auth_request(p, AUTH_PASSKEY, 0);
            else check_result(esp_ble_passkey_reply(e->ble_security.ble_req.bd_addr, false, 0), "reject unknown BLE passkey");
            break;
        case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
            p = peer_for(e->ble_security.key_notif.bd_addr, ESP_HID_TRANSPORT_BLE);
            if (pairing_allowed(p))
                ESP_LOGI(TAG, "type %06" PRIu32 " then Enter ON THE BLUETOOTH KEYBOARD", e->ble_security.key_notif.passkey);
            break;
        case ESP_GAP_BLE_AUTH_CMPL_EVT:
            p = peer_for(e->ble_security.auth_cmpl.bd_addr, ESP_HID_TRANSPORT_BLE);
            if (p) {
                p->secure = e->ble_security.auth_cmpl.success;
                p->auth = AUTH_NONE;
                p->pin_replied = false;
                p->bonded = ble_bonded(p->address);
                ESP_LOGI(TAG, "input %u BLE encryption=%u reason=%u", (unsigned)(p - peers) + 1,
                         p->secure, e->ble_security.auth_cmpl.fail_reason);
                if (!p->secure) {
                    p->paused = true;
                    release_peer(p);
                    if (p->dev) check_result(esp_hidh_dev_close(p->dev), "close insecure BLE input");
                }
                request_mouse_interval(p);
                queue_state();
            }
            break;
        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            p = peer_for(e->update_conn_params.bda, ESP_HID_TRANSPORT_BLE);
            if (p) {
                p->interval_pending = false;
                unsigned slot = (unsigned)(p - peers) + 1;
                if (e->update_conn_params.status == ESP_BT_STATUS_SUCCESS) {
                    p->interval = e->update_conn_params.conn_int;
                    ESP_LOGI(TAG, "input %u negotiated BLE interval=%u.%03ums latency=%u timeout=%ums",
                             slot, e->update_conn_params.conn_int * 5u / 4u,
                             e->update_conn_params.conn_int % 4u * 250u,
                             e->update_conn_params.latency, e->update_conn_params.timeout * 10u);
                    request_mouse_interval(p);
                } else {
                    ESP_LOGE(TAG, "input %u BLE parameter update failed: status=%u",
                             slot, e->update_conn_params.status);
                }
            }
            break;
        default: break;
    }
    unlock();
}

static void gattc_event(esp_gattc_cb_event_t event, esp_gatt_if_t interface, esp_ble_gattc_cb_param_t *e) {
    if (event == ESP_GATTC_CONNECT_EVT) {
        lock();
        peer_t *p = peer_for(e->connect.remote_bda, ESP_HID_TRANSPORT_BLE);
        if (p && !p->paused)
            check_result(esp_ble_set_encryption(p->address, ESP_BLE_SEC_ENCRYPT), "BLE link encryption");
        unlock();
    }
    esp_hidh_gattc_event_handler(event, interface, e);
}

static void uart_task(void *arg) {
    (void)arg;
    uint8_t bytes[256], wire[HUB_LINK_WIRE_MAX];
    for (;;) {
        int length = uart_read_bytes(UART_NUM_1, bytes, sizeof(bytes), pdMS_TO_TICKS(1));
        lock();
        uart_event_t event;
        while (xQueueReceive(uart_events, &event, 0) == pdTRUE) {
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
                event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR) {
                nonce = 0;
                session = random_nonzero();
                memset(&parser, 0, sizeof(parser));
                xQueueReset(snapshots);
                check_result(uart_flush_input(UART_NUM_1), "flush UART");
                ESP_LOGE(TAG, "UART RX fault type=%d; link handshake reset", event.type);
                length = 0;
            }
        }
        for (int i = 0; i < length; ++i) {
            hub_link_frame_t f;
            if (!hub_link_feed(&parser, bytes[i], &f)) continue;
            if (f.type == HUB_LINK_REQUEST) {
                if (nonce != f.nonce) {
                    nonce = f.nonce;
                    sequence = 0;
                    memset(&pico_output, 0, sizeof(pico_output));
                    xQueueReset(snapshots);
                    ESP_LOGI(TAG, "Pico UART handshake established");
                    sent_at = now_ms() - HUB_LINK_HEARTBEAT_MS;
                }
                request_at = now_ms();
            } else if (f.type == HUB_LINK_OUTPUT &&
                       !hub_link_receive_output(&pico_output, &f, nonce, session, now_ms())) {
                ESP_LOGW(TAG, "discarded invalid/stale Pico computer telemetry");
            } else if(f.type==HUB_LINK_MANAGE_REPLY && pc_attempts && f.nonce==nonce && f.session==session &&
                      !memcmp(f.payload,pc_request.payload,4)){
                snprintf(operation,sizeof(operation),f.payload[4]?"Computer slot deleted":"Computer delete rejected by Pico (busy/stale identity)");
                pc_attempts=0;pc_request.type=0;
            }
        }
        if (nonce && (uint32_t)(now_ms() - request_at) >= HUB_LINK_TIMEOUT_MS) {
            nonce = 0;
            xQueueReset(snapshots);
            ESP_LOGW(TAG, "Pico heartbeat lost; discarding queued input");
        }
        classic_encryption_event_t encrypted;
        while (xQueueReceive(encryption_events, &encrypted, 0) == pdTRUE) {
            peer_t *p = peer_for(encrypted.address, ESP_HID_TRANSPORT_BT);
            if (!p || !input_classic_encryption_current(p->selected, p->paused, p->classic_acl_connected,
                    p->mapped, p->encryption_pending, p->encryption_token, encrypted.generation)) {
                ESP_LOGW(TAG, "stale Classic encryption result ignored");
                continue;
            }
            p->encryption_pending = false;
            p->secure = p->classic_acl_encrypted = encrypted.encrypted;
            ESP_LOGI(TAG, "input %u Classic encryption completion: status=%u stack-flags=0x%02x encrypted=%u",
                     (unsigned)(p - peers) + 1, encrypted.status, encrypted.flags, encrypted.encrypted);
            if (!encrypted.connected) {
                p->classic_acl_connected = false;
                ++p->acl_generation;
                recover_peer(p, "link disappeared during encryption; wait for HID CLOSE before retry");
            } else if (!p->secure) {
                p->paused = encrypted.security_failure;
                recover_peer(p, p->paused ? "security failure; explicit action required" : "temporary encryption failure");
            }
            queue_state();
        }
        for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i) {
            peer_t *p = &peers[i];
            if (!p->selected || p->paused || p->recovering || p->transport != ESP_HID_TRANSPORT_BT ||
                !p->classic_acl_connected || !p->mapped || p->secure || p->allow_open || p->encryption_pending)
                continue;
            p->encryption_pending = true;
            p->encryption_token = ++encryption_token;
            esp_err_t error = hub_esp_classic_encrypt(p->address, p->encryption_token);
            check_result(error, "request Classic link encryption");
            ESP_LOGI(TAG, "input %u requesting actual Classic encryption; pairing window unchanged", i + 1);
            if (error != ESP_OK) {
                recover_peer(p, "encryption request could not be queued");
            }
        }
        hub_link_input_t snapshot[HUB_LINK_SLOTS];
        bool control_due=nonce && ((pc_request.type && (!pc_attempts || now_ms()-pc_request_at>=1000)) ||
                                   now_ms()-ui_sent_at>=500);
        bool send = nonce && !control_due && xQueueReceive(snapshots, snapshot, 0) == pdTRUE;
        if (!send && nonce && !control_due && (uint32_t)(now_ms() - sent_at) >= HUB_LINK_HEARTBEAT_MS) {
            snapshot_locked(snapshot);
            send = true;
        }
        size_t count = 0;
        if (send) {
            hub_link_frame_t f = {.nonce = nonce, .session = session, .sequence = sequence++};
            hub_link_pack(&f, snapshot);
            count = hub_link_encode(&f, wire);
            sent_at = now_ms();
        } else if(nonce && pc_request.type && (!pc_attempts || now_ms()-pc_request_at>=1000)) {
            if(pc_attempts>=4){
                snprintf(operation,sizeof(operation),"Computer delete acknowledgement missing; refresh slots before retry");
                pc_attempts=0;pc_request.type=0;
            }else{
                pc_request.nonce=nonce;pc_request.session=session;
                count=hub_link_encode(&pc_request,wire);++pc_attempts;pc_request_at=now_ms();
            }
        } else if(nonce && now_ms()-ui_sent_at>=500){
            hub_link_frame_t f={.type=HUB_LINK_UI,.length=1,.nonce=nonce,.session=session,.sequence=ui_sequence++};
            f.payload[0]=management_open() || trial.phase;
            count=hub_link_encode(&f,wire);ui_sent_at=now_ms();
        }
        unlock();
        if (count) {
            int written = uart_write_bytes(UART_NUM_1, wire, count);
            lock();
            if (written != (int)count) {
                nonce = 0;
                xQueueReset(snapshots);
                ESP_LOGE(TAG, "UART TX failed; wrote=%d expected=%u", written, (unsigned)count);
            } else ++sent_count;
            unlock();
        }
    }
}

static input_reconnect_view_t reconnect_view(const peer_t *p) {
    return (input_reconnect_view_t){
        .selected = p->selected, .paused = p->paused, .pending = p->pending,
        .recovering = p->recovering, .opening = p->open_running, .device = p->dev != NULL, .mapped = p->mapped,
        .forwarding = forwarding_allowed(p), .classic = p->transport == ESP_HID_TRANSPORT_BT,
        .acl = p->classic_acl_connected, .profile_requested = p->profile_requested,
        .pairing = pairing_window_open(p), .started_at = p->opened_at, .retry_at = p->retry_at
    };
}

static void open_input(unsigned index) {
    uint8_t address[6], transport, type;
    lock();
    peer_t *p = &peers[index];
    if (p->paused || p->recovering || !p->selected) {
        p->open_running = false;
        if (!p->dev && !p->classic_acl_connected) retry_peer(p);
        unlock();
        return;
    }
    memcpy(address, p->address, sizeof(address));
    transport = p->transport;
    type = p->address_type;
    unlock();
    ESP_LOGI(TAG, "connecting input %u " ESP_BD_ADDR_STR, index + 1, ESP_BD_ADDR_HEX(address));
    esp_hidh_dev_t *result = esp_hidh_dev_open(address, transport, type);
    lock();
    p = &peers[index];
    p->open_running = false;
    /* OPEN/CLOSE callbacks own device lifetime; never store a returned SDK pointer. */
    if ((!result || !p->pending || p->recovering) && !p->dev && !p->classic_acl_connected) {
        release_peer(p);
        retry_peer(p);
        ESP_LOGW(TAG, "input %u open ended without live HID; retry in %" PRIu32 "ms",
                 index + 1, input_reconnect_backoff(p->reconnect_failures));
    }
    unlock();
}

static void ble_open_task(void *arg) {
    (void)arg;
    unsigned index;
    for (;;) if (xQueueReceive(ble_opens, &index, portMAX_DELAY) == pdTRUE) open_input(index);
}

static void connection_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        lock();
        uint32_t now = now_ms();
        for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i) {
            peer_t *p = &peers[i];
            if (p->auth && due(now, p->pair_until)) reject_auth(p);
            if (p->pending && p->mapped && forwarding_allowed(p)) {
                p->pending = false;
                p->pair_until = 0;
                p->reconnect_failures = 0;
                p->profile_failures = 0;
            }
            if (p->recovering) {
                if (!p->dev && !p->classic_acl_connected && !p->open_running) retry_peer(p);
                else if ((uint32_t)(now - p->recovery_at) >= 5000) {
                    p->recovery_at = now;
                    recover_peer(p, "waiting for disconnect/open worker");
                }
                continue;
            }
            input_reconnect_view_t view = reconnect_view(p);
            if (input_reconnect_plan(&view, now, false) == INPUT_RECOVER) {
                reject_auth(p);
                recover_peer(p, "connection/security deadline; automatic cleanup and retry");
            }
        }
        int classic_index = -1;
        if (!scanning) {
            for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i) {
                peer_t *p = &peers[i];
                if(management_open() && !p->mapped && !(trial.phase==2 && trial.role==i+1))continue;
                if (p->transport == ESP_HID_TRANSPORT_BT && !classic_ready) continue;
                bool busy = false;
                for (unsigned j = 0; j < HUB_LINK_SLOTS; ++j) {
                    input_reconnect_view_t other = reconnect_view(&peers[j]);
                    if (j != i && peers[j].transport == p->transport) busy |= input_reconnect_busy(&other);
                }
                input_reconnect_view_t view = reconnect_view(p);
                input_reconnect_action_t action = input_reconnect_plan(&view, now, busy);
                if (action != INPUT_OPEN && action != INPUT_ATTACH) continue;
                if (action == INPUT_ATTACH && !classic_admitted(p)) {
                    p->paused = true;
                    recover_peer(p, "incoming ACL has no bond/pairing consent");
                    continue;
                }
                p->pending = p->open_running = p->profile_requested = true;
                p->opened_at = now;
                if (p->transport == ESP_HID_TRANSPORT_BT && p->classic_acl_connected) {
                    p->secure = p->classic_acl_encrypted;
                    ESP_LOGI(TAG, "input %u attaching HID to existing Classic ACL", i + 1);
                } else {
                    p->classic_incoming = false;
                    p->secure = false;
                }
                if (p->transport == ESP_HID_TRANSPORT_BT) classic_index = (int)i;
                else {
                    if (xQueueSend(ble_opens, &i, 0) != pdTRUE) {
                        p->open_running = false;
                        ESP_LOGE(TAG, "BLE open worker queue full; retry without losing bond");
                        retry_peer(p);
                    }
                }
            }
        }
        unlock();
        if (classic_index >= 0) open_input((unsigned)classic_index);
    }
}

static bool web_snapshot(hub_web_snapshot_t *out) {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(5)) != pdTRUE) return false;
    memset(out, 0, sizeof(*out));
    uint32_t now = now_ms();
    out->uptime_ms = now;
    out->packets = sent_count;
    out->overflows = overflow_count;
    out->uart_online = nonce && (uint32_t)(now - request_at) < HUB_LINK_TIMEOUT_MS;
    out->pico_received = pico_output.valid;
    out->pico_age_ms = now - pico_output.received_at;
    out->pico_fresh = out->uart_online && hub_link_output_fresh(&pico_output, nonce, session, now);
    out->output = pico_output.value;
    out->records=saved_inputs;
    out->auto_enrollment=auto_enrollment;
    out->records_fault=records_fault;
    out->management_open=management_open();out->trial=trial.phase!=0;out->scanning=scanning||requested_scan;
    out->scan_waiting=requested_scan!=SCAN_NONE;
    out->trial_role=trial.role;
    out->trial_phase=trial.phase;
    if(trial.phase)memcpy(out->trial_address,trial.target.address,6);
    out->remaining_ms=out->management_open?management_until-now:0;
    if(out->management_open)memcpy(out->token,management_token,sizeof(out->token));
    memcpy(out->operation,operation,sizeof(out->operation));
    out->candidate_count=candidate_count;
    memcpy(out->candidates,candidates,sizeof(candidates));
    for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i) {
        const peer_t *p = &peers[i];
        hub_web_input_t *input = &out->inputs[i];
        input->saved = p->selected;
        input->bonded = p->selected && p->bonded;
        input->connected = p->dev && p->mapped;
        input->encrypted = p->secure;
        input->hid = p->mapped;
        input->ready = p->selected && !p->paused && !p->recovering && p->mapped && forwarding_allowed(p) &&
            !(trial.phase && trial.role==i+1);
        input->paused = p->paused;
        input->pending = p->pending;
        input->transport = p->transport;
        input->roles = p->hid.roles;
        input->interval = p->interval;
        input->reports = p->reports;
        memcpy(input->address, p->address, 6);
        memcpy(input->name, p->name, sizeof(input->name));
        if (!trial.phase || trial.role != i + 1) {
            uint8_t record = saved_inputs.active[i];
            if (record < HUB_SAVED_INPUTS) {
                input->record_slot = record + 1;
                if (!input->name[0])
                    snprintf(input->name, sizeof(input->name), "%s", saved_inputs.slots[record].name);
            }
        }
        if(pairing_allowed(p) && p->auth){
            out->auth=p->auth;out->auth_slot=i+1;out->auth_number=p->auth_number;
        }
    }
    unlock();
    return true;
}

static esp_err_t persist_records(const hub_input_records_t *next) {
    uint8_t bytes[HUB_INPUT_RECORD_BYTES],readback[HUB_INPUT_RECORD_BYTES];
    hub_records_encode(next,bytes);
    esp_err_t error=nvs_set_blob(store,"records5",bytes,sizeof(bytes));
    if(error==ESP_OK)error=nvs_commit(store);
    size_t length=sizeof(readback);
    if(error==ESP_OK)error=nvs_get_blob(store,"records5",readback,&length);
    if(error==ESP_OK && (length!=sizeof(bytes) || memcmp(bytes,readback,sizeof(bytes))))error=ESP_ERR_INVALID_RESPONSE;
    check_result(error,"save/verify input registry");
    if(error==ESP_OK)saved_inputs=*next;
    else records_fault=true;
    return error;
}
static void update_saved_name(const uint8_t address[6],const char *name) {
    if(!name || !name[0] || records_fault)return;
    char normalized[HUB_INPUT_NAME_BYTES];hub_input_name_copy(normalized,name);
    for(unsigned i=0;i<HUB_SAVED_INPUTS;++i){
        hub_saved_input_t *s=&saved_inputs.slots[i];
        if(!s->used || memcmp(s->address,address,6) || !strcmp(s->name,normalized))continue;
        memcpy(s->name,normalized,sizeof(s->name));record_names_dirty=true;
    }
}
static void configure_peer(unsigned role,const hub_saved_input_t *record,bool pause) {
    peer_t *p=&peers[role-1];uint8_t epoch=p->input.epoch+1;uint32_t generation=p->acl_generation+1;
    memset(p,0,sizeof(*p));p->input.epoch=epoch;p->acl_generation=generation;
    if(!record || !record->used)return;
    p->selected=true;p->paused=pause;p->transport=record->transport;p->address_type=record->type;
    p->allow_open=record->allow_open;memcpy(p->address,record->address,6);
    snprintf(p->name,sizeof(p->name),"%s",record->name);
    p->bonded=p->transport==ESP_HID_TRANSPORT_BT?classic_bonded(p->address):ble_bonded(p->address);
}
static bool peer_idle(const peer_t *p){return !p->dev && !p->open_running && !p->classic_acl_connected && !p->recovering;}
static void stop_scan(void) {
    requested_scan=SCAN_NONE;
    if(!scanning)return;
    if(active_scan==SCAN_CLASSIC)check_result(esp_bt_gap_cancel_discovery(),"cancel Classic scan");
    else if(active_scan==SCAN_BLE && !scan_ble_pending)check_result(esp_ble_gap_stop_scanning(),"cancel BLE scan");
}
static esp_err_t scan_inputs(bool classic) {
    if(scanning || requested_scan || trial.phase)return ESP_ERR_INVALID_STATE;
    requested_scan=classic?SCAN_CLASSIC:SCAN_BLE;
    scan_requested_at=now_ms();
    if(!auto_enrollment)management_until=now_ms()+INPUT_PAIRING_WINDOW_MS;
    snprintf(operation,sizeof(operation),"扫描已排队：等待旧连接结束后自动开始，无需反复点击");
    ESP_LOGI(TAG,"authorized %s discovery queued; old records retained",classic?"Classic":"BLE");
    return ESP_OK;
}
static void start_queued_scan(void) {
    if(!requested_scan || !management_open() || trial.phase)return;
    for(unsigned i=0;i<2;++i)
        if(peers[i].open_running || peers[i].recovering || (peers[i].pending&&!peers[i].mapped)){
            if(now_ms()-scan_requested_at>=INPUT_RECONNECT_TIMEOUT_MS+10000u){
                requested_scan=SCAN_NONE;
                snprintf(operation,sizeof(operation),"旧连接未能结束，扫描未开始；原记录保留，可重新授权重试");
                ESP_LOGE(TAG,"discovery wait timed out; no target changed");
            }
            return;
        }
    bool classic=requested_scan==SCAN_CLASSIC;
    active_scan=requested_scan;requested_scan=SCAN_NONE;
    if(!auto_enrollment || auto_stage==1)candidate_count=0;
    auto_scan_complete=false;
    esp_err_t error;
    scanning=true;scan_started_at=now_ms();
    if(classic)error=esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,4,0);
    else{
        esp_ble_scan_params_t params={.scan_type=BLE_SCAN_TYPE_ACTIVE,.own_addr_type=BLE_ADDR_TYPE_PUBLIC,
            .scan_filter_policy=BLE_SCAN_FILTER_ALLOW_ALL,.scan_interval=0x50,.scan_window=0x30,
            .scan_duplicate=BLE_SCAN_DUPLICATE_ENABLE};
        scan_ble_pending=true;error=esp_ble_gap_set_scan_params(&params);
    }
    if(error!=ESP_OK){
        scanning=scan_ble_pending=false;active_scan=SCAN_NONE;auto_enrollment=false;
        check_result(error,"start authorized discovery");
        snprintf(operation,sizeof(operation),"扫描启动失败：%s；原记录未改变",esp_err_to_name(error));
    }else snprintf(operation,sizeof(operation),auto_enrollment?
        "正在自动扫描 %s；唯一新键鼠将自动配对":
        "正在扫描 %s；结束后请选中新鼠标",classic?"Classic":"BLE");
}
static void close_management(void) {
    auto_enrollment=false;auto_stage=0;
    management_until=0;management_token[0]=0;stop_scan();
    if(trial.phase)trial.abort=true;
    for(unsigned i=0;i<2;++i){peers[i].pair_until=0;if(peers[i].auth)reject_auth(&peers[i]);}
}
static void open_management(void) {
    if(trial.phase || (hub_link_output_fresh(&pico_output,nonce,session,now_ms()) &&
        (pico_output.value.flags&HUB_LINK_OUTPUT_PAIRING))){
        snprintf(operation,sizeof(operation),"Finish current input/PC pairing before opening management");return;
    }
    management_until=now_ms()+120000;candidate_count=0;
    auto_enrollment=true;auto_stage=1;auto_committed=false;auto_scan_complete=false;auto_retry_at=now_ms();
    snprintf(management_token,sizeof(management_token),"%08" PRIX32 "%08" PRIX32,random_nonzero(),random_nonzero());
    if(scanning || requested_scan){
        auto_enrollment=false;
        snprintf(operation,sizeof(operation),"管理已授权；当前扫描忙，请扫描结束后重新长按或手动管理");
    }else{
        scan_inputs(false);
        snprintf(operation,sizeof(operation),"自动录入已开启：扫描明确的新键鼠，排除已保存设备和本机");
    }
    ESP_LOGI(TAG,"physical button opened management; existing bonds unchanged");
}
static esp_err_t enqueue_management(const hub_web_command_t *command) {
    lock();
    bool allowed=management_open() && !strcmp(command->token,management_token);
    esp_err_t result=!allowed?ESP_ERR_INVALID_STATE:
        xQueueSend(management_commands,command,0)==pdTRUE?ESP_OK:ESP_ERR_NO_MEM;
    unlock();return result;
}
static void remove_input_bond(const hub_saved_input_t *s) {
    if(!s->used)return;
    uint8_t address[6];memcpy(address,s->address,6);
    check_result(s->transport==ESP_HID_TRANSPORT_BT?esp_bt_gap_remove_bond_device(address):
                 esp_ble_remove_bond_device(address),"remove explicitly deleted input bond");
}
static esp_err_t handle_management(const hub_web_command_t *cmd) {
    if(!management_open() || strcmp(cmd->token,management_token) || records_fault)return ESP_ERR_INVALID_STATE;
    if(!strcmp(cmd->action,"cancel")){close_management();snprintf(operation,sizeof(operation),"已取消，旧设备记录保留");return ESP_OK;}
    if(!strcmp(cmd->action,"manual")){
        auto_enrollment=false;auto_stage=0;stop_scan();
        snprintf(operation,sizeof(operation),"手动管理已开启，扫描停止后可删除记录或选择设备");
        return ESP_OK;
    }
    if(!strcmp(cmd->action,"rename")){
        if(!cmd->slot || cmd->slot>HUB_SAVED_INPUTS || trial.phase || !cmd->name[0])return ESP_ERR_INVALID_ARG;
        const hub_saved_input_t *s=&saved_inputs.slots[cmd->slot-1];char address[18];
        snprintf(address,sizeof(address),ESP_BD_ADDR_STR,ESP_BD_ADDR_HEX(s->address));
        if(!s->used || strcasecmp(address,cmd->address))return ESP_ERR_INVALID_ARG;
        for(unsigned i=0;cmd->name[i];++i)if((unsigned char)cmd->name[i]<32 || cmd->name[i]==127)return ESP_ERR_INVALID_ARG;
        hub_input_records_t next=saved_inputs;
        hub_input_name_copy(next.slots[cmd->slot-1].name,cmd->name);
        esp_err_t error=persist_records(&next);
        if(error==ESP_OK){
            for(unsigned i=0;i<2;++i)if(saved_inputs.active[i]==cmd->slot-1)
                snprintf(peers[i].name,sizeof(peers[i].name),"%s",next.slots[cmd->slot-1].name);
            snprintf(operation,sizeof(operation),"设备名称已保存");
        }
        return error;
    }
    if(!strcmp(cmd->action,"scan")){
        if(strcmp(cmd->transport,"Classic")&&strcmp(cmd->transport,"BLE"))return ESP_ERR_INVALID_ARG;
        auto_enrollment=false;auto_stage=0;
        return scan_inputs(!strcmp(cmd->transport,"Classic"));
    }
    if(!strcmp(cmd->action,"pc-delete")){
        if(pc_request.type || !cmd->slot || cmd->slot>7 ||
           !hub_link_output_fresh(&pico_output,nonce,session,now_ms()) ||
           (pico_output.value.flags&HUB_LINK_OUTPUT_PAIRING))return ESP_ERR_INVALID_STATE;
        const hub_link_pc_t *pc=&pico_output.value.computers[cmd->slot-1];
        char address[18];snprintf(address,sizeof(address),ESP_BD_ADDR_STR,ESP_BD_ADDR_HEX(pc->address));
        if(!(pc->flags&HUB_LINK_PC_RESERVED) || strcasecmp(address,cmd->address))return ESP_ERR_INVALID_ARG;
        pc_request=(hub_link_frame_t){.type=HUB_LINK_MANAGE,.length=12,.sequence=++pc_sequence};
        uint32_t id=random_nonzero();for(unsigned i=0;i<4;++i)pc_request.payload[i]=(uint8_t)(id>>(8*i));
        pc_request.payload[4]=cmd->slot;pc_request.payload[5]=pc->address_type;
        memcpy(pc_request.payload+6,pc->address,6);pc_attempts=0;
        snprintf(operation,sizeof(operation),"Computer delete pending Pico acknowledgement");return ESP_OK;
    }
    if(!strcmp(cmd->action,"confirm") || !strcmp(cmd->action,"passkey")){
        if(!cmd->slot || cmd->slot>2)return ESP_ERR_INVALID_ARG;
        peer_t *p=&peers[cmd->slot-1];if(!pairing_allowed(p))return ESP_ERR_INVALID_STATE;
        esp_err_t error=ESP_ERR_INVALID_ARG;
        if(!strcmp(cmd->action,"confirm")&&p->auth==AUTH_CONFIRM&&
           (!strcmp(cmd->value,"yes")||!strcmp(cmd->value,"no")))
            error=p->transport==ESP_HID_TRANSPORT_BT?esp_bt_gap_ssp_confirm_reply(p->address,!strcmp(cmd->value,"yes")):
                esp_ble_confirm_reply(p->address,!strcmp(cmd->value,"yes"));
        else if(!strcmp(cmd->action,"passkey")&&p->auth==AUTH_PASSKEY&&strlen(cmd->value)==6&&strspn(cmd->value,"0123456789")==6)
            error=p->transport==ESP_HID_TRANSPORT_BT?esp_bt_gap_ssp_passkey_reply(p->address,true,strtoul(cmd->value,NULL,10)):
                esp_ble_passkey_reply(p->address,true,strtoul(cmd->value,NULL,10));
        if(error==ESP_OK)p->auth=AUTH_NONE;
        return error;
    }
    if(trial.phase || scanning || requested_scan)return ESP_ERR_INVALID_STATE;
    hub_saved_input_t target={0};unsigned role=0,record=255;uint8_t kind=1;
    if(!strcmp(cmd->action,"delete") || !strcmp(cmd->action,"activate")){
        if(!cmd->slot || cmd->slot>5 || !saved_inputs.slots[cmd->slot-1].used)return ESP_ERR_INVALID_ARG;
        record=cmd->slot-1;target=saved_inputs.slots[record];role=target.role;
        char address[18];snprintf(address,sizeof(address),ESP_BD_ADDR_STR,ESP_BD_ADDR_HEX(target.address));
        if(strcasecmp(address,cmd->address))return ESP_ERR_INVALID_ARG;
        if(!strcmp(cmd->action,"delete")) {
            if(saved_inputs.active[role-1]!=record){
                hub_input_records_t next=saved_inputs;hub_records_forget(&next,record);
                esp_err_t error=persist_records(&next);
                if(error==ESP_OK){remove_input_bond(&target);snprintf(operation,sizeof(operation),"Input record deleted");}
                return error;
            }
            kind=2;
        }else kind=3;
    }else if(!strcmp(cmd->action,"connect")){
        if(cmd->candidate>=candidate_count || (cmd->slot!=1 && cmd->slot!=2))return ESP_ERR_INVALID_ARG;
        const hub_web_candidate_t *c=&candidates[cmd->candidate];role=cmd->slot;
        char address[18];snprintf(address,sizeof(address),ESP_BD_ADDR_STR,ESP_BD_ADDR_HEX(c->address));
        if(strcasecmp(address,cmd->address))return ESP_ERR_INVALID_ARG;
        bool full=true;for(unsigned i=0;i<5;++i)full&=saved_inputs.slots[i].used;
        if(full && !cmd->replace)return ESP_ERR_INVALID_ARG;
        target=(hub_saved_input_t){.used=true,.role=role,.transport=c->transport,.type=c->type};
        memcpy(target.address,c->address,6);
        memcpy(target.name,c->name,sizeof(target.name));
        for(unsigned i=0;i<5;++i)if(saved_inputs.slots[i].used && !memcmp(saved_inputs.slots[i].address,target.address,6)){
            if(saved_inputs.slots[i].role!=role)return ESP_ERR_INVALID_ARG;
            record=i;
        }
    }else return ESP_ERR_INVALID_ARG;
    peer_t *p=&peers[role-1];
    if(peers[2-role].selected && !memcmp(peers[2-role].address,target.address,6))return ESP_ERR_INVALID_ARG;
    trial=(input_trial_t){.phase=1,.kind=kind,.role=role,.record=record,.target=target,
        .was_bonded=target.transport==ESP_HID_TRANSPORT_BT?classic_bonded(target.address):ble_bonded(target.address)};
    p->paused=true;p->pair_until=0;reject_auth(p);recover_peer(p,"authorized slot operation");
    management_until=now_ms()+INPUT_PAIRING_WINDOW_MS;
    ESP_LOGI(TAG,"authorized new target role=%u " ESP_BD_ADDR_STR "; old saved input retained until verified",
             role,ESP_BD_ADDR_HEX(target.address));
    snprintf(operation,sizeof(operation),"正在结束旧设备连接；新设备验证成功后才保存，旧记录保留");return ESP_OK;
}
static void auto_enrollment_tick(void) {
    if(!auto_enrollment || !management_open() || trial.phase || records_fault)return;
    if(scanning || requested_scan)return;
    if(auto_stage==1 || auto_stage==2){
        if(!auto_scan_complete)return;
        auto_scan_complete=false;
        if(auto_stage==1){
            auto_stage=2;scan_inputs(true);return;
        }
        auto_stage=3;
    }
    if(auto_stage!=3)return;
    int mouse=hub_auto_candidate(candidates,candidate_count,&saved_inputs,HUB_LINK_MOUSE);
    int keyboard=hub_auto_candidate(candidates,candidate_count,&saved_inputs,HUB_LINK_KEYBOARD);
    if(mouse==-2 || keyboard==-2){
        auto_enrollment=false;auto_stage=0;
        snprintf(operation,sizeof(operation),"发现多只同类新设备，已暂停自动录入；请网页明确选择，旧记录未改变");
        ESP_LOGW(TAG,"automatic enrollment ambiguous; no guessed selection");
        return;
    }
    int selected=mouse>=0?mouse:keyboard;
    if(selected<0){
        if(auto_committed){
            close_management();return;
        }
        if((int32_t)(now_ms()-auto_retry_at)<0)return;
        auto_retry_at=now_ms()+1000;
        auto_stage=1;auto_scan_complete=false;scan_inputs(false);
        snprintf(operation,sizeof(operation),"未发现唯一可识别的新键鼠，继续扫描；请让新鼠标保持配对");
        return;
    }
    hub_web_command_t command={0};strcpy(command.action,"connect");
    memcpy(command.token,management_token,sizeof(command.token));
    command.slot=mouse>=0?2:1;command.candidate=(unsigned)selected;command.replace=true;
    snprintf(command.address,sizeof(command.address),ESP_BD_ADDR_STR,ESP_BD_ADDR_HEX(candidates[selected].address));
    esp_err_t error=handle_management(&command);
    if(error!=ESP_OK){
        auto_enrollment=false;auto_stage=0;
        snprintf(operation,sizeof(operation),"自动录入请求未执行：%s，旧设备记录保留",esp_err_to_name(error));
        check_result(error,"automatic target enrollment");
    }else ESP_LOGI(TAG,"automatic new input selected name=%s role=%u " ESP_BD_ADDR_STR,
                   candidates[selected].name,command.slot,ESP_BD_ADDR_HEX(candidates[selected].address));
}
static void manage_tick(void) {
    if(!trial.phase)for(unsigned role=0;role<2;++role)
        if(saved_inputs.active[role]==255 && peers[role].selected && peer_idle(&peers[role]))
            configure_peer(role+1,NULL,false);
    if(management_until && !management_open()){close_management();snprintf(operation,sizeof(operation),"管理窗口已到期；未完成的新设备取消并恢复旧记录");}
    hub_web_command_t cmd;
    if(xQueueReceive(management_commands,&cmd,0)==pdTRUE){
        if(strcmp(cmd.action,"cancel")&&strcmp(cmd.action,"confirm")&&strcmp(cmd.action,"passkey")&&
           strcmp(cmd.action,"manual")){
            auto_enrollment=false;auto_stage=0;
        }
        esp_err_t error=handle_management(&cmd);
        if(error!=ESP_OK){snprintf(operation,sizeof(operation),"操作未执行：%s；旧记录未删除",esp_err_to_name(error));check_result(error,"management action");}
    }
    auto_enrollment_tick();
    start_queued_scan();
    if(active_scan && scanning && now_ms()-scan_started_at>=20000u){
        stop_scan();
        snprintf(operation,sizeof(operation),"扫描结束事件超时；等待扫描停止，旧记录未改变");
        ESP_LOGE(TAG,"discovery completion timeout");
        scan_started_at=now_ms();
    }
    if(!trial.phase){
        if(record_names_dirty && !records_fault){
            hub_input_records_t next=saved_inputs;
            if(persist_records(&next)==ESP_OK)record_names_dirty=false;
        }
        return;
    }
    peer_t *p=&peers[trial.role-1];
    if(trial.abort && trial.phase!=4){p->paused=true;p->pair_until=0;reject_auth(p);recover_peer(p,"cancel/rollback provisional input");trial.phase=4;}
    if(trial.phase==1 && peer_idle(p)){
        if(trial.kind==2){
            hub_input_records_t next=saved_inputs;
            hub_records_forget(&next,trial.record);
            if(persist_records(&next)==ESP_OK){
                remove_input_bond(&trial.target);configure_peer(trial.role,NULL,false);trial.phase=0;
                snprintf(operation,sizeof(operation),"Input record deleted");return;
            }
            trial.phase=4;
        }else{
            configure_peer(trial.role,&trial.target,false);
            if(trial.kind==1){
                management_until=now_ms()+INPUT_PAIRING_WINDOW_MS;
                p->pair_until=management_until;
            }
            trial.phase=2;
            ESP_LOGI(TAG,"input trial started role=%u " ESP_BD_ADDR_STR "; encrypted HID and role required",
                     trial.role,ESP_BD_ADDR_HEX(p->address));
            snprintf(operation,sizeof(operation),"正在连接选中的新设备；等待加密和 HID 验证");
        }
    }
    if(trial.phase==2){
        uint8_t required=trial.role==1?HUB_LINK_KEYBOARD:HUB_LINK_MOUSE;
        if(p->mapped&&p->secure&&!p->paused&&!p->recovering){
            if(!(p->hid.roles&required)){trial.abort=true;snprintf(operation,sizeof(operation),"设备不是所选键鼠类型，正在恢复旧输入");return;}
            hub_input_records_t previous=saved_inputs,next=saved_inputs;
            if(p->name[0])hub_input_name_copy(trial.target.name,p->name);
            int slot=hub_records_insert(&next,&trial.target);
            if(slot<0 || persist_records(&next)!=ESP_OK){trial.abort=true;return;}
            for(unsigned i=0;i<5;++i)if(previous.slots[i].used &&
                (!next.slots[i].used || memcmp(previous.slots[i].address,next.slots[i].address,6))){
                remove_input_bond(&previous.slots[i]);
                for(unsigned role=0;role<2;++role)if(role+1!=trial.role && previous.active[role]==i){
                    peers[role].paused=true;recover_peer(&peers[role],"oldest saved input evicted");
                }
            }
            p->pair_until=0;trial.phase=0;led_ack_until=now_ms()+1000;record_names_dirty=false;
            if(auto_enrollment){auto_committed=true;auto_stage=3;}
            else{management_until=0;management_token[0]=0;}
            ESP_LOGI(TAG,"new input saved and active: role=%u record=%d " ESP_BD_ADDR_STR,
                     trial.role,slot+1,ESP_BD_ADDR_HEX(p->address));
            snprintf(operation,sizeof(operation),"新设备已验证、保存并启用；可在已保存输入中切回旧设备");queue_state();
        }else if(p->paused || p->reconnect_failures>=3){trial.abort=true;snprintf(operation,sizeof(operation),"新设备连接失败，正在恢复旧输入记录");}
    }
    if(trial.phase==4 && peer_idle(p)){
        if(!trial.was_bonded && trial.kind!=2)remove_input_bond(&trial.target);
        uint8_t active=saved_inputs.active[trial.role-1];
        configure_peer(trial.role,active<5?&saved_inputs.slots[active]:NULL,false);
        close_management();
        trial.phase=0;
        snprintf(operation,sizeof(operation),records_fault?"Input storage verification failed; changes locked; inspect storage":
                 "已恢复原设备记录，新设备未保存；请确认鼠标仍处于蓝牙配对模式");queue_state();
    }
    if(!trial.phase)for(unsigned role=0;role<2;++role)
        if(saved_inputs.active[role]==255 && peers[role].selected && peer_idle(&peers[role]))
            configure_peer(role+1,NULL,false);
}
static void controls_task(void *arg) {
    (void)arg;hub_ui_button_t button={0};
    hub_ui_fault_clock_t fault_clock={0};
    uint32_t started=now_ms();bool web_toggle=false,web_on=false;
    for(;;){
        vTaskDelay(pdMS_TO_TICKS(20));lock();
        uint32_t now=now_ms();hub_ui_action_t action=hub_ui_tick(&button,gpio_get_level(GPIO_NUM_0)==0,now);
        if(action==HUB_UI_LONG){open_management();web_on=true;}
        else if(action==HUB_UI_DOUBLE)web_toggle=true;
        else if(action==HUB_UI_SHORT){
            if(management_open()||trial.phase){close_management();snprintf(operation,sizeof(operation),"Cancelled by button");}
            else led_ack_until=now+1000;
        }
        manage_tick();
        status_led_mode_t mode=status_led_mode_locked(now);
        bool fault_led=hub_ui_fault_tick(&fault_clock,mode==LED_FAULT&&!(button.long_press&&button.stable),now);
        bool led=now-started<500 || (led_ack_until&&!due(now,led_ack_until)&&(mode==LED_READY||mode==LED_IDLE)) ||
            (mode==LED_FAULT?fault_led:status_led_level(mode,now));
        if(button.long_press&&button.stable)led=hub_ui_led(HUB_LED_PAIR,now);
        unlock();gpio_set_level(STATUS_LED_PIN,led);
        if(web_toggle||web_on){
            check_result(hub_web_set_enabled(web_on?true:!hub_web_enabled()),"physical button hotspot control");
            web_toggle=web_on=false;
        }
    }
}

static bool parse_address(const char *s, uint8_t address[6]) {
    if (strlen(s) != 17) return false;
    for (unsigned i = 0; i < 6; ++i) {
        char part[3] = {s[i * 3], s[i * 3 + 1], 0}, *end;
        for (unsigned k = 0; k < 2; ++k)
            if (!((part[k] >= '0' && part[k] <= '9') || (part[k] >= 'a' && part[k] <= 'f') ||
                  (part[k] >= 'A' && part[k] <= 'F'))) return false;
        address[i] = (uint8_t)strtoul(part, &end, 16);
        if (*end || (i < 5 && s[i * 3 + 2] != ':')) return false;
    }
    return true;
}
static int slot_number(const char *s) {
    return s && s[0] >= '1' && s[0] <= '2' && s[1] == 0 ? s[0] - '1' : -1;
}
static esp_err_t save_target(unsigned i, const uint8_t *address, uint8_t transport, uint8_t type, bool allow_open) {
    hub_saved_input_t input={.used=true,.role=i+1,.transport=transport,.type=type,.allow_open=allow_open};
    memcpy(input.address,address,6);hub_input_records_t next=saved_inputs;
    if(hub_records_insert(&next,&input)<0)return ESP_ERR_INVALID_ARG;
    return persist_records(&next);
}
static int console_command(int argc, char **argv) {
    if (!strcmp(argv[0], "web")) return hub_web_command(argc, argv);
    int result = 1;
    lock();
    int slot = argc > 1 ? slot_number(argv[1]) : -1;
    peer_t *p = slot >= 0 ? &peers[slot] : NULL;
    if(!strcmp(argv[0],"enroll")){
        if(argc==1 || (argc==2 && !strcmp(argv[1],"auto"))){
            open_management();
            result=management_open()?0:1;
        }else if(argc==2 && !strcmp(argv[1],"cancel")){
            close_management();snprintf(operation,sizeof(operation),"操作员已取消自动录入，旧记录保留");result=0;
        }
        else if(argc==2 && !strcmp(argv[1],"manual")){
            open_management();auto_enrollment=false;auto_stage=0;stop_scan();
            result=management_open()?0:1;
        }
        printf("Enrollment: management=%u automatic=%u %s\n",management_open(),auto_enrollment,operation);
    } else if (!strcmp(argv[0], "status") && argc == 1) {
        printf("ESP32 input firmware " HUB_WEB_FIRMWARE ", UART1 TX=%d RX=%d baud=460800, Pico-link=%u packets=%" PRIu32
               " overflows=%" PRIu32 "\n", LINK_UART_TX_PIN, LINK_UART_RX_PIN,
               !!nonce, sent_count, overflow_count);
        printf("Classic HID security: encryption required, MITM not required (unless explicitly open-connected)\n");
        esp_bt_gap_profile_status_t gap_status;
        esp_err_t gap_error = esp_bt_gap_get_profile_status(&gap_status);
        if (gap_error == ESP_OK)
            printf("Classic GAP: connectable=%u discoverable=%u; new pairing requires explicit target window\n",
                   gap_status.conn_mode == ESP_BT_CONNECTABLE,
                   gap_status.disc_mode != ESP_BT_NON_DISCOVERABLE);
        else check_result(gap_error, "read Classic GAP profile status");
        printf("Classic legacy PIN: fixed %s during explicit pairing window; status LED GPIO%d=%s\n",
               CLASSIC_FIXED_PIN, STATUS_LED_PIN, status_led_name(status_led_mode_locked(now_ms())));
        printf("BLE mouse interval target: 7.500ms; preserve negotiated latency/timeout\n");
        printf("Input management: open=%u remaining-ms=%" PRIu32 " scan-wait=%u scanning=%u candidates=%u trial-phase=%u trial-role=%u storage-fault=%u\n",
               management_open(),management_open()?management_until-now_ms():0,requested_scan,scanning,candidate_count,
               trial.phase,trial.role,records_fault);
        printf("Auto enrollment=%u stage=%u BOOT-pressed=%u\n",auto_enrollment,auto_stage,gpio_get_level(GPIO_NUM_0)==0);
        printf("Input operation: %s\n",operation);
        if(trial.phase)printf("Trial target: " ESP_BD_ADDR_STR " transport=%u; previous registry retained\n",
                              ESP_BD_ADDR_HEX(trial.target.address),trial.target.transport);
        for(unsigned i=0;i<HUB_SAVED_INPUTS;++i)if(saved_inputs.slots[i].used)
            printf("Saved input %u role=%u active=%u order=%u " ESP_BD_ADDR_STR " name=%s\n",i+1,saved_inputs.slots[i].role,
                   saved_inputs.active[saved_inputs.slots[i].role-1]==i,saved_inputs.slots[i].order,
                   ESP_BD_ADDR_HEX(saved_inputs.slots[i].address),saved_inputs.slots[i].name);
        printf("Pico computer telemetry: received=%u fresh=%u age-ms=%" PRIu32 "\n",
               pico_output.valid, hub_link_output_fresh(&pico_output, nonce, session, now_ms()),
               pico_output.valid ? now_ms() - pico_output.received_at : 0);
        for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i) {
            printf("input %u selected=%u paused=%u pending=%u HID=%u encrypted=%u roles=%u reports=%" PRIu32
                   " invalid=%" PRIu32 " allow-unencrypted=%u " ESP_BD_ADDR_STR "\n", i + 1, peers[i].selected, peers[i].paused,
                   peers[i].pending, peers[i].mapped, peers[i].secure, peers[i].seen_roles,
                   peers[i].reports, peers[i].invalid, peers[i].allow_open, ESP_BD_ADDR_HEX(peers[i].address));
            if (peers[i].selected && peers[i].transport == ESP_HID_TRANSPORT_BT)
                printf("input %u Classic bonded=%u incoming=%u incoming-connections=%" PRIu32
                       " ACL=%u ACL-encrypted=%u encryption-request=%u generation=%" PRIu32 "\n",
                       i + 1, classic_bonded(peers[i].address), peers[i].classic_incoming,
                       peers[i].incoming_connections, peers[i].classic_acl_connected,
                       peers[i].classic_acl_encrypted, peers[i].encryption_pending, peers[i].acl_generation);
            printf("input %u reconnect: worker=%u recovering=%u profile-request=%u failures=%u retry-in-ms=%" PRIu32 "\n",
                   i + 1, peers[i].open_running, peers[i].recovering, peers[i].profile_requested,
                   peers[i].reconnect_failures, due(now_ms(), peers[i].retry_at) ? 0 : peers[i].retry_at - now_ms());
            if (peers[i].transport == ESP_HID_TRANSPORT_BLE && peers[i].dev) {
                esp_gap_conn_params_t params;
                esp_err_t error = esp_ble_get_current_conn_params(peers[i].address, &params);
                if (error == ESP_OK)
                    printf("input %u BLE interval=%u.%03ums latency=%u timeout=%ums update-pending=%u attempts=%u\n",
                           i + 1, params.interval * 5u / 4u,
                           params.interval % 4u * 250u, params.latency, params.timeout * 10u,
                           peers[i].interval_pending, peers[i].interval_attempts);
                else
                    printf("input %u BLE parameters unavailable: %s\n",
                           i + 1, esp_err_to_name(error));
            }
        }
        result = 0;
    } else if (!strcmp(argv[0], "scan") && argc == 2) {
        bool busy = scanning;
        for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i)
            busy |= peers[i].pending || peers[i].open_running || peers[i].recovering;
        if (busy) ESP_LOGE(TAG, "scan/connection already in progress");
        else if (!strcmp(argv[1], "classic")) {
            scanning = true;
            esp_err_t e = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 5, 0);
            if (e != ESP_OK) scanning = false;
            check_result(e, "Classic scan");
            result = e != ESP_OK;
        } else if (!strcmp(argv[1], "ble")) {
            esp_ble_scan_params_t params = {
                .scan_type = BLE_SCAN_TYPE_ACTIVE, .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL, .scan_interval = 0x50,
                .scan_window = 0x30, .scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE
            };
            scan_ble_pending = scanning = true;
            esp_err_t e = esp_ble_gap_set_scan_params(&params);
            if (e != ESP_OK) scan_ble_pending = scanning = false;
            check_result(e, "BLE scan params");
            result = e != ESP_OK;
        }
    } else if (p && ((!strcmp(argv[0], "connect") && (argc == 4 || argc == 5)) ||
                    (!strcmp(argv[0], "open-connect") && argc == 3))) {
        uint8_t address[6], type = 0, transport = ESP_HID_TRANSPORT_MAX;
        bool allow_open = !strcmp(argv[0], "open-connect");
        if (allow_open) transport = ESP_HID_TRANSPORT_BT;
        if (argc == 4 && !strcmp(argv[2], "classic")) transport = ESP_HID_TRANSPORT_BT;
        if (argc == 5 && !strcmp(argv[2], "ble") &&
            (!strcmp(argv[3], "public") || !strcmp(argv[3], "random"))) {
            transport = ESP_HID_TRANSPORT_BLE;
            type = !strcmp(argv[3], "random") ? BLE_ADDR_TYPE_RANDOM : BLE_ADDR_TYPE_PUBLIC;
        }
        if (transport == ESP_HID_TRANSPORT_MAX || !parse_address(argv[argc - 1], address))
            ESP_LOGE(TAG, "invalid transport/address");
        else if (p->dev || p->pending || p->open_running || p->recovering || p->classic_acl_connected || scanning)
            ESP_LOGE(TAG, "disconnect first; wait for CLOSE/open worker and scan completion");
        else if (peers[1 - slot].selected && !memcmp(address, peers[1 - slot].address, 6))
            ESP_LOGE(TAG, "peer already belongs to the other input slot");
        else if (p->selected && (p->transport != transport || memcmp(p->address, address, 6)))
            ESP_LOGE(TAG, "forget the existing target before replacing it");
        else {
            esp_err_t e = save_target(slot, address, transport, type, allow_open);
            check_result(e, "persist input target");
            if (e == ESP_OK) {
                p->selected = true; p->paused = false;
                p->transport = transport; p->address_type = type;
                p->allow_open = allow_open;
                if (allow_open)
                    ESP_LOGW(TAG, "input %d EXPLICIT unencrypted/unauthenticated forwarding consent saved; MAC can be spoofed", slot + 1);
                memcpy(p->address, address, 6);
                p->bonded = transport == ESP_HID_TRANSPORT_BT ? classic_bonded(address) : ble_bonded(address);
                p->pair_until = now_ms() + INPUT_PAIRING_WINDOW_MS;
                p->retry_at = now_ms();
                result = 0;
            }
        }
    } else if (!strcmp(argv[0], "disconnect") && p && argc == 2) {
        p->paused = true; p->pair_until = now_ms();
        reject_auth(p);
        release_peer(p);
        recover_peer(p, "user disconnect");
        result = 0;
    } else if (!strcmp(argv[0], "forget") && p && argc == 2) {
        if (p->dev || p->pending || p->open_running || p->classic_acl_connected)
            ESP_LOGE(TAG, "disconnect first and wait for CLOSE/open worker");
        else {
            hub_input_records_t next=saved_inputs;
            uint8_t record=next.active[slot];
            if(record<5)hub_records_forget(&next,record);
            esp_err_t e=persist_records(&next);
            check_result(e, "forget target");
            if (e == ESP_OK) {
                if (p->selected) check_result(p->transport == ESP_HID_TRANSPORT_BT
                    ? esp_bt_gap_remove_bond_device(p->address) : esp_ble_remove_bond_device(p->address), "remove input bond");
                release_peer(p);
                p->selected = false;
                p->bonded = false;
                p->name[0] = 0;
                p->classic_acl_connected = p->classic_acl_encrypted = p->classic_incoming = false;
                ++p->acl_generation;
                result = 0;
            }
        }
    } else if (p && argc == 3 && pairing_allowed(p)) {
        esp_err_t e = ESP_ERR_INVALID_ARG;
        if (!strcmp(argv[0], "confirm") && p->auth == AUTH_CONFIRM &&
            (!strcmp(argv[2], "yes") || !strcmp(argv[2], "no"))) {
            bool yes = !strcmp(argv[2], "yes");
            e = p->transport == ESP_HID_TRANSPORT_BT ? esp_bt_gap_ssp_confirm_reply(p->address, yes)
                                                   : esp_ble_confirm_reply(p->address, yes);
        } else if (!strcmp(argv[0], "passkey") && p->auth == AUTH_PASSKEY &&
                   strlen(argv[2]) == 6 && strspn(argv[2], "0123456789") == 6) {
            uint32_t value = (uint32_t)strtoul(argv[2], NULL, 10);
            e = p->transport == ESP_HID_TRANSPORT_BT ? esp_bt_gap_ssp_passkey_reply(p->address, true, value)
                                                   : esp_ble_passkey_reply(p->address, true, value);
        }
        check_result(e, "pairing reply");
        if (e == ESP_OK) { p->auth = AUTH_NONE; result = 0; }
    }
    if (result) ESP_LOGW(TAG, "command rejected; enter help for syntax/state requirements");
    unlock();
    return result;
}

void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init()); /* Never erase stored bonds to hide an NVS error. */
    ESP_ERROR_CHECK(nvs_open("dual_bt", NVS_READWRITE, &store));
    mutex = xSemaphoreCreateMutex();
    snapshots = xQueueCreate(64, sizeof(hub_link_input_t) * HUB_LINK_SLOTS);
    encryption_events = xQueueCreate(8, sizeof(classic_encryption_event_t));
    ble_opens = xQueueCreate(HUB_LINK_SLOTS, sizeof(unsigned));
    management_commands=xQueueCreate(4,sizeof(hub_web_command_t));
    configASSERT(mutex && snapshots && encryption_events && ble_opens && management_commands);
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << STATUS_LED_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&led));
    ESP_ERROR_CHECK(gpio_set_level(STATUS_LED_PIN, 0));
    gpio_config_t boot={.pin_bit_mask=1ULL<<GPIO_NUM_0,.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE,
        .pull_down_en=GPIO_PULLDOWN_DISABLE,.intr_type=GPIO_INTR_DISABLE};
    ESP_ERROR_CHECK(gpio_config(&boot));
    session = random_nonzero();
    for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i) {
        uint8_t record[10];
        size_t length = sizeof(record);
        esp_err_t e = nvs_get_blob(store, i ? "input2" : "input1", record, &length);
        if (e == ESP_ERR_NVS_NOT_FOUND) continue;
        ESP_ERROR_CHECK(e);
        if (length != sizeof(record) || record[0] != 2 ||
            (record[1] != ESP_HID_TRANSPORT_BT && record[1] != ESP_HID_TRANSPORT_BLE) || record[2] > 1 ||
            record[9] > 1 || (record[9] && record[1] != ESP_HID_TRANSPORT_BT)) {
            ESP_LOGE(TAG, "invalid stored input %u; refusing automatic connection", i + 1);
            continue;
        }
        peers[i].selected = true;
        peers[i].transport = record[1]; peers[i].address_type = record[2];
        peers[i].allow_open = record[9] != 0;
        if (peers[i].allow_open) ESP_LOGW(TAG, "input %u restores EXPLICIT unencrypted Classic consent", i + 1);
        memcpy(peers[i].address, record + 3, 6);
    }
    hub_records_init(&saved_inputs);
    uint8_t registry[HUB_INPUT_RECORD_BYTES];size_t registry_size=sizeof(registry);
    esp_err_t registry_error=nvs_get_blob(store,"records5",registry,&registry_size);
    if(registry_error==ESP_OK){
        if(!hub_records_decode_sized(&saved_inputs,registry,registry_size)){
            ESP_LOGE(TAG,"invalid input registry; no legacy fallback");abort();
        }
        for(unsigned role=0;role<2;++role){
            memset(&peers[role],0,sizeof(peers[role]));uint8_t active=saved_inputs.active[role];
            if(active<5){
                hub_saved_input_t *s=&saved_inputs.slots[active];peer_t *p=&peers[role];
                p->selected=true;p->transport=s->transport;p->address_type=s->type;p->allow_open=s->allow_open;
                memcpy(p->address,s->address,6);
                snprintf(p->name,sizeof(p->name),"%s",s->name);
            }
        }
        if(registry_size==HUB_INPUT_RECORD_V1_BYTES)record_names_dirty=true;
    }else if(registry_error==ESP_ERR_NVS_NOT_FOUND){
        for(unsigned role=0;role<2;++role)if(peers[role].selected){
            peer_t *p=&peers[role];hub_saved_input_t s={.used=true,.role=role+1,.transport=p->transport,
                .type=p->address_type,.allow_open=p->allow_open};memcpy(s.address,p->address,6);
            if(hub_records_insert(&saved_inputs,&s)<0){
                ESP_LOGE(TAG,"legacy input migration rejected; refusing to drop a saved input");
                abort();
            }
        }
    }else ESP_ERROR_CHECK(registry_error);
    uart_config_t uart = {.baud_rate = 460800, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT};
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, LINK_UART_TX_PIN, LINK_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(gpio_set_pull_mode(LINK_UART_RX_PIN, GPIO_PULLUP_ONLY));
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, 2048, 1024, 32, &uart_events, 0));
    esp_bt_controller_config_t controller = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&controller));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i)
        if (peers[i].selected) peers[i].bonded = peers[i].transport == ESP_HID_TRANSPORT_BT
            ? classic_bonded(peers[i].address) : ble_bonded(peers[i].address);
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(classic_gap));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(ble_gap));
    ESP_ERROR_CHECK(esp_ble_gattc_register_callback(gattc_event));
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name("Pico Dual BT Input"));
    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE));
    esp_bt_io_cap_t bt_io = ESP_BT_IO_CAP_IO;
    ESP_ERROR_CHECK(esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &bt_io, sizeof(bt_io)));
    esp_bt_pin_code_t empty_pin = {0};
    ESP_ERROR_CHECK(esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, empty_pin));
    uint8_t io = ESP_IO_CAP_IO, auth = ESP_LE_AUTH_BOND, key_size = 16;
    uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io, 1));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, 1));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, 1));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, 1));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, 1));
    esp_hidh_config_t hid = {.callback = hid_event, .event_stack_size = 6144};
    ESP_ERROR_CHECK(esp_hidh_init(&hid));
    configASSERT(xTaskCreate(uart_task, "pico_uart", 4096, NULL, 5, NULL) == pdPASS);
    configASSERT(xTaskCreate(ble_open_task, "ble_open", 6144, NULL, 3, NULL) == pdPASS);
    configASSERT(xTaskCreate(connection_task, "hid_connect", 6144, NULL, 3, NULL) == pdPASS);
    check_result(hub_web_init(web_snapshot,enqueue_management), "SoftAP/HTTP startup (Bluetooth stays enabled)");
    configASSERT(xTaskCreate(controls_task,"hub_controls",8192,NULL,2,NULL)==pdPASS);
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    esp_console_dev_uart_config_t console = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    config.prompt = "bt-input>";
    config.max_cmdline_length = 128;
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&console, &config, &repl));
    const struct { const char *name, *help; } commands[] = {
        {"enroll","enroll [auto|manual|cancel]; USB operator authorization, same admission path as BOOT long press"},
        {"status", "Board, UART link and two input-slot counters"},
        {"web", "web [on|off] OR web password <8..63 printable ASCII>; saved locally, USB only"},
        {"scan", "scan ble|classic (list only; never automatically authorizes a peer)"},
        {"connect", "connect <1|2> classic <MAC> OR connect <1|2> ble public|random <MAC>; 120s pairing window"},
        {"open-connect", "open-connect <1|2> <MAC>; EXPLICIT saved consent to UNENCRYPTED/UNAUTHENTICATED Classic HID"},
        {"disconnect", "disconnect <1|2>; pause reconnect and release input"},
        {"forget", "forget <1|2>; only after disconnect; removes this target/bond"},
        {"confirm", "confirm <1|2> yes|no; compare the pairing numbers first"},
        {"passkey", "passkey <1|2> <six digits>; requested passkey only"}
    };
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        esp_console_cmd_t c = {.command = commands[i].name, .help = commands[i].help, .func = console_command};
        ESP_ERROR_CHECK(esp_console_cmd_register(&c));
    }
    ESP_LOGI(TAG, HUB_WEB_FIRMWARE " ESP32 INPUT; UART1 TX=%d RX=%d 460800; LED GPIO%d; SoftAP read-only",
             LINK_UART_TX_PIN, LINK_UART_RX_PIN, STATUS_LED_PIN);
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
