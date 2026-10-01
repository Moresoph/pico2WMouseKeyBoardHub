#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static const char *TAG = "dual-bt";
static const char CLASSIC_FIXED_PIN[] = "123456";
enum {
    LINK_UART_TX_PIN = GPIO_NUM_13,
    LINK_UART_RX_PIN = GPIO_NUM_14,
    STATUS_LED_PIN = GPIO_NUM_2
};
enum { AUTH_NONE, AUTH_CONFIRM, AUTH_PASSKEY, AUTH_PIN };
enum { INPUT_PAIRING_WINDOW_MS = 120000, INPUT_CONNECT_TIMEOUT_MS = 40000 };
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
    uint8_t address[6], transport, address_type, auth, seen_roles;
    uint8_t interval_attempts;
    uint32_t pair_until, retry_at, opened_at, reports, invalid, incoming_connections;
    esp_hidh_dev_t *dev;
    hub_remote_hid_t hid;
    hub_link_input_t input;
} peer_t;
static peer_t peers[HUB_LINK_SLOTS];
static SemaphoreHandle_t mutex;
static QueueHandle_t snapshots, uart_events;
static nvs_handle_t store;
static uint32_t nonce, session, request_at, sent_at, sent_count, overflow_count;
static uint16_t sequence;
static bool classic_ready, scanning, scan_ble_pending;
static hub_link_parser_t parser;

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
        if (!p->mapped || !forwarding_allowed(p) || p->paused) {
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
    p->classic_incoming = false;
    uint8_t epoch = p->input.epoch + 1u;
    memset(&p->input, 0, sizeof(p->input));
    p->input.epoch = epoch;
    queue_state();
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
    if (pairing) return LED_PAIRING;
    if (fault) return LED_FAULT;
    if (selected && all_ready) return LED_READY;
    return selected ? LED_RECONNECTING : LED_IDLE;
}

static bool status_led_level(status_led_mode_t mode, uint32_t now) {
    uint32_t phase;
    switch (mode) {
        case LED_ENTER_FIXED_PIN:
            return (now / 100u) % 2u == 0;
        case LED_PAIRING:
            return (now / 500u) % 2u == 0;
        case LED_READY:
            return true;
        case LED_FAULT:
            phase = now % 2000u;
            return phase < 100u || (phase >= 200u && phase < 300u);
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
    if (!p || p->paused) {
        if (event == ESP_HIDH_OPEN_EVENT && e->open.status == ESP_OK) {
            ESP_LOGW(TAG, "reject HID open from unselected/paused input " ESP_BD_ADDR_STR,
                     ESP_BD_ADDR_HEX(address));
            check_result(esp_hidh_dev_close(dev), "close unselected input");
        }
        if (p && (event == ESP_HIDH_CLOSE_EVENT ||
                  (event == ESP_HIDH_OPEN_EVENT && e->open.status != ESP_OK))) {
            p->dev = NULL;
            p->pending = false;
            release_peer(p);
        }
        unlock();
        return;
    }
    unsigned slot = (unsigned)(p - peers) + 1;
    if (event == ESP_HIDH_OPEN_EVENT) {
        if (e->open.status != ESP_OK) {
            p->dev = NULL;
            p->pending = false;
            p->retry_at = now_ms() + 5000;
            release_peer(p);
            ESP_LOGE(TAG, "input %u open failed: %s", slot, esp_err_to_name(e->open.status));
        } else {
            if (transport == ESP_HID_TRANSPORT_BT && !classic_admitted(p)) {
                p->dev = dev;
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
        p->dev = NULL;
        p->pending = false;
        p->retry_at = now_ms() + 5000;
        release_peer(p);
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
            if (e->acl_conn_cmpl_stat.stat == ESP_BT_STATUS_SUCCESS && (!p || (!p->pending && !p->dev))) {
                if (classic_admitted(p)) begin_classic_incoming(p);
                else ESP_LOGW(TAG, "unsolicited Classic ACL is not authorized for HID; pairing requests will be rejected");
            }
            break;
        case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
            p = peer_for(e->acl_disconn_cmpl_stat.bda, ESP_HID_TRANSPORT_BT);
            if (p) {
                ESP_LOGI(TAG, "input %u Classic ACL disconnected reason=%u",
                         (unsigned)(p - peers) + 1, e->acl_disconn_cmpl_stat.reason);
                if (p->classic_incoming && !p->dev) {
                    p->pending = false;
                    p->retry_at = now_ms() + 5000;
                }
                release_peer(p);
            }
            break;
        case ESP_BT_GAP_DISC_RES_EVT:
            ESP_LOGI(TAG, "scan classic " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(e->disc_res.bda));
            break;
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
            scanning = e->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED;
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
                ESP_LOGI(TAG, "input %u Classic auth status=%u", (unsigned)(p - peers) + 1, e->auth_cmpl.stat);
                if (e->auth_cmpl.stat != ESP_BT_STATUS_SUCCESS) {
                    p->paused = true;
                    release_peer(p);
                    if (p->dev) check_result(esp_hidh_dev_close(p->dev), "close unauthenticated input");
                }
            }
            break;
        case ESP_BT_GAP_ENC_CHG_EVT:
            p = peer_for(e->enc_chg.bda, ESP_HID_TRANSPORT_BT);
            if (p) {
                p->secure = e->enc_chg.enc_mode != ESP_BT_ENC_MODE_OFF;
                if (!forwarding_allowed(p)) release_peer(p);
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
                esp_err_t error = e->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS
                    ? esp_ble_gap_start_scanning(6) : ESP_FAIL;
                if (error != ESP_OK) scanning = false;
                check_result(error, "BLE scan");
            }
            break;
        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            if (e->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) scanning = false;
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
                ESP_LOGE(TAG, "BLE scan start status=%u", e->scan_start_cmpl.status);
            }
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
            if (hub_link_feed(&parser, bytes[i], &f) && f.type == HUB_LINK_REQUEST) {
                if (nonce != f.nonce) {
                    nonce = f.nonce;
                    sequence = 0;
                    xQueueReset(snapshots);
                    ESP_LOGI(TAG, "Pico UART handshake established");
                    sent_at = now_ms() - HUB_LINK_HEARTBEAT_MS;
                }
                request_at = now_ms();
            }
        }
        if (nonce && (uint32_t)(now_ms() - request_at) >= HUB_LINK_TIMEOUT_MS) {
            nonce = 0;
            xQueueReset(snapshots);
            ESP_LOGW(TAG, "Pico heartbeat lost; discarding queued input");
        }
        hub_link_input_t snapshot[HUB_LINK_SLOTS];
        bool send = nonce && xQueueReceive(snapshots, snapshot, 0) == pdTRUE;
        if (!send && nonce && (uint32_t)(now_ms() - sent_at) >= HUB_LINK_HEARTBEAT_MS) {
            snapshot_locked(snapshot);
            send = true;
        }
        size_t count = 0;
        if (send) {
            hub_link_frame_t f = {.nonce = nonce, .session = session, .sequence = sequence++};
            hub_link_pack(&f, snapshot);
            count = hub_link_encode(&f, wire);
            sent_at = now_ms();
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

static void connection_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        lock();
        uint32_t now = now_ms();
        bool busy = scanning;
        for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i) {
            peer_t *p = &peers[i];
            if (p->auth && due(now, p->pair_until)) reject_auth(p);
            if (p->pending && p->mapped && forwarding_allowed(p)) {
                p->pending = false;
                p->pair_until = 0;
            }
            if (p->pending && (uint32_t)(now - p->opened_at) > INPUT_CONNECT_TIMEOUT_MS &&
                (!p->pair_until || due(now, p->pair_until))) {
                p->paused = true;
                reject_auth(p);
                release_peer(p);
                if (p->dev) check_result(esp_hidh_dev_close(p->dev), "connection timeout close");
                ESP_LOGE(TAG, "input %u connection/security timed out; paused", i + 1);
                p->pending = false;
            }
            busy |= p->pending;
        }
        int index = -1;
        if (!busy) {
            for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i) {
                peer_t *p = &peers[i];
                if (p->selected && !p->paused && !p->dev && due(now, p->retry_at) &&
                    (p->transport != ESP_HID_TRANSPORT_BT || classic_ready)) { index = (int)i; break; }
            }
        }
        uint8_t address[6], transport = 0, type = 0;
        if (index >= 0) {
            peer_t *p = &peers[index];
            p->pending = true;
            p->classic_incoming = false;
            p->secure = false;
            p->opened_at = now;
            memcpy(address, p->address, 6);
            transport = p->transport;
            type = p->address_type;
        }
        bool led_on = status_led_level(status_led_mode_locked(now), now);
        unlock();
        gpio_set_level(STATUS_LED_PIN, led_on);
        if (index < 0) continue;
        ESP_LOGI(TAG, "connecting input %d " ESP_BD_ADDR_STR, index + 1, ESP_BD_ADDR_HEX(address));
        esp_hidh_dev_t *dev = esp_hidh_dev_open(address, transport, type);
        lock();
        peer_t *p = &peers[index];
        if (!dev) {
            p->pending = false;
            p->retry_at = now_ms() + 5000;
            ESP_LOGW(TAG, "input %d open submission failed; retry in 5s", index + 1);
        } else if (p->pending && !p->dev) p->dev = dev;
        unlock();
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
    uint8_t record[10] = {2, transport, type};
    memcpy(record + 3, address, 6);
    record[9] = allow_open;
    esp_err_t e = nvs_set_blob(store, i ? "input2" : "input1", record, sizeof(record));
    if (e == ESP_OK) e = nvs_commit(store);
    return e;
}
static int console_command(int argc, char **argv) {
    int result = 1;
    lock();
    int slot = argc > 1 ? slot_number(argv[1]) : -1;
    peer_t *p = slot >= 0 ? &peers[slot] : NULL;
    if (!strcmp(argv[0], "status") && argc == 1) {
        printf("ESP32 input firmware 0.5.1-bonded-reconnect, UART1 TX=%d RX=%d baud=460800, Pico-link=%u packets=%" PRIu32
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
        for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i) {
            printf("input %u selected=%u paused=%u pending=%u HID=%u encrypted=%u roles=%u reports=%" PRIu32
                   " invalid=%" PRIu32 " allow-unencrypted=%u " ESP_BD_ADDR_STR "\n", i + 1, peers[i].selected, peers[i].paused,
                   peers[i].pending, peers[i].mapped, peers[i].secure, peers[i].seen_roles,
                   peers[i].reports, peers[i].invalid, peers[i].allow_open, ESP_BD_ADDR_HEX(peers[i].address));
            if (peers[i].selected && peers[i].transport == ESP_HID_TRANSPORT_BT)
                printf("input %u Classic bonded=%u incoming=%u incoming-connections=%" PRIu32 "\n",
                       i + 1, classic_bonded(peers[i].address), peers[i].classic_incoming,
                       peers[i].incoming_connections);
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
        for (unsigned i = 0; i < HUB_LINK_SLOTS; ++i) busy |= peers[i].pending;
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
        else if (p->dev || p->pending || scanning) ESP_LOGE(TAG, "disconnect first; wait for CLOSE and scan completion");
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
                p->pair_until = now_ms() + INPUT_PAIRING_WINDOW_MS;
                p->retry_at = now_ms();
                result = 0;
            }
        }
    } else if (!strcmp(argv[0], "disconnect") && p && argc == 2) {
        p->paused = true; p->pair_until = now_ms();
        reject_auth(p);
        release_peer(p);
        if (p->dev) check_result(esp_hidh_dev_close(p->dev), "disconnect");
        result = 0;
    } else if (!strcmp(argv[0], "forget") && p && argc == 2) {
        if (p->dev || p->pending) ESP_LOGE(TAG, "disconnect first and wait for CLOSE");
        else {
            esp_err_t e = nvs_erase_key(store, slot ? "input2" : "input1");
            if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;
            if (e == ESP_OK) e = nvs_commit(store);
            check_result(e, "forget target");
            if (e == ESP_OK) {
                if (p->selected) check_result(p->transport == ESP_HID_TRANSPORT_BT
                    ? esp_bt_gap_remove_bond_device(p->address) : esp_ble_remove_bond_device(p->address), "remove input bond");
                release_peer(p);
                p->selected = false;
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
    configASSERT(mutex && snapshots);
    gpio_config_t led = {
        .pin_bit_mask = 1ULL << STATUS_LED_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&led));
    ESP_ERROR_CHECK(gpio_set_level(STATUS_LED_PIN, 0));
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
    configASSERT(xTaskCreate(connection_task, "hid_connect", 6144, NULL, 3, NULL) == pdPASS);
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    esp_console_dev_uart_config_t console = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    config.prompt = "bt-input>";
    config.max_cmdline_length = 128;
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&console, &config, &repl));
    const struct { const char *name, *help; } commands[] = {
        {"status", "Board, UART link and two input-slot counters"},
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
    ESP_LOGI(TAG, "0.5.1-bonded-reconnect ESP32 INPUT; UART1 TX=%d RX=%d 460800; LED GPIO%d; Wi-Fi disabled",
             LINK_UART_TX_PIN, LINK_UART_RX_PIN, STATUS_LED_PIN);
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
