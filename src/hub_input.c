#include "hub_input.h"
#include "hub_input_policy.h"
#include "hub_config.h"
#include "hub_router.h"
#include "hub_security.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "btstack_config.h"
#include "btstack_event.h"
#include "btstack_hid_parser.h"
#include "btstack_run_loop.h"
#include "btstack_tlv.h"
#include "gap.h"
#include "hci.h"
#include "ble/gatt-service/hids_host.h"
#include "ble/sm.h"
#include "ble/le_device_db.h"
#include "classic/hid_host.h"

#define INPUT_DESCRIPTOR_BYTES 4096u
#define INPUT_REPORTS 16u
#define INPUT_SERVICES 3u
#define INPUT_CONNECT_MS 20000u
#define INPUT_DISCOVERY_MS 30000u
#define INPUT_SCAN_MS 15000u
#define INPUT_RETRIES 3u
#define INPUT_SLOW_RETRY_MS 30000u
#define INPUT_ENROLL_SCAN_MS 4000u
#define INPUT_CANDIDATES 12u
#define INPUT_SCAN_PEERS 16u
/* Application-owned "PHI1"/"PHI2", unrelated to BTstack's key/database tags. */
#define INPUT_TARGET_TAG 0x50484931u

_Static_assert(HUB_INPUT_COUNT == 2, "This module provides exactly two input slots");
_Static_assert(MAX_NR_HID_HOST_CONNECTIONS >= 2, "Two Classic HID hosts required");
_Static_assert(MAX_NR_HIDS_HOSTS >= 2, "Two BLE HIDS hosts required");

typedef enum {
    INPUT_EMPTY, INPUT_CONNECTING, INPUT_SECURITY, INPUT_DISCOVERING,
    INPUT_READY, INPUT_CLOSING, INPUT_RETRY, INPUT_FAILED
} input_state_t;

typedef struct {
    uint16_t id;
    uint16_t bits;
    uint8_t service;
    bool keyboard;
    bool mouse;
    uint8_t keys[32];
    uint8_t buttons;
} input_report_t;

typedef struct {
    uint16_t offset;
    uint16_t len;
    bool report_ids;
} input_descriptor_t;

typedef struct {
    bool selected;
    bool classic;
    bool remove;
    bool pairing_authorized;
    bool auth_failed;
    bool ever_ready;
    bool disconnect_sent;
    bool warned_report;
    bool retry_allowed;
    bool protocol_pending;
    bool candidate;
    bool stored;
    bool forget;
    bool security_complete;
    bool hid_validated;
    bool identity_valid;
    bool paused;
    bool storage_error;
    bool bond_address_valid;
    bool open_test;
    uint32_t test_reports;
    uint8_t bond_address_type;
    bd_addr_t bond_address;
    uint8_t roles;
    uint8_t identity_type;
    bd_addr_t identity;
    uint8_t address_type;
    bd_addr_t address;
    hci_con_handle_t handle;
    uint16_t cid;
    input_state_t state;
    uint8_t last_error;
    uint8_t last_auth_status;
    uint8_t last_hid_status;
    uint8_t last_disconnect_reason;
    uint8_t retries;
    uint32_t deadline;
    uint8_t service_count;
    uint8_t report_count;
    uint16_t descriptor_used;
    input_descriptor_t descriptors[INPUT_SERVICES];
    input_report_t reports[INPUT_REPORTS];
    uint8_t descriptor[INPUT_DESCRIPTOR_BYTES];
} input_slot_t;

static input_slot_t inputs[HUB_INPUT_COUNT];
static uint8_t classic_descriptors[HUB_INPUT_COUNT * INPUT_DESCRIPTOR_BYTES + 1u];
/* The spare byte makes a truncated oversized second BLE map fail our 4K limit. */
static uint8_t le_descriptors[HUB_INPUT_COUNT * INPUT_DESCRIPTOR_BYTES + 1u];
static btstack_packet_callback_registration_t input_hci_registration;
static btstack_packet_callback_registration_t input_sm_registration;
static btstack_timer_source_t input_timer;
static bool input_started;
static bool open_bridge_enabled;
static int establishing = -1;
static enum { SCAN_OFF, SCAN_CLASSIC, SCAN_LE, SCAN_CLASSIC_STOPPING } scanning;
static uint32_t scan_deadline;
static bool classic_inquiry_active;
static const btstack_tlv_t *input_tlv;
static void *input_tlv_context;
static bool corrupt_target[HUB_INPUT_COUNT];
static bool enrollment_active;
static bool enrollment_pending;
static uint32_t enrollment_deadline;
static uint32_t enrollment_next_scan;
static bool enrollment_next_classic;
static unsigned forgetting;
static bool resolving_list_supported;
typedef struct {
    bool classic;
    uint8_t type;
    bd_addr_t address;
} input_candidate_t;
static input_candidate_t candidates[INPUT_CANDIDATES];
static unsigned candidate_count;
static int pending_candidate = -1;
typedef struct {
    bool used;
    bool eligible;
    uint8_t type;
    bd_addr_t address;
    uint32_t seen;
} input_scan_peer_t;
static input_scan_peer_t scan_peers[INPUT_SCAN_PEERS];
static unsigned next_scan_peer;

static void input_hid_event(uint8_t, uint16_t, uint8_t *, uint16_t);
static void input_hids_event(uint8_t, uint16_t, uint8_t *, uint16_t);
static void enrollment_tick(uint32_t now);

static unsigned slot_index(const input_slot_t *slot) {
    return (unsigned)(slot - inputs);
}

static bool expired(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static input_slot_t *slot_for_handle(hci_con_handle_t handle) {
    if (handle == HCI_CON_HANDLE_INVALID) return NULL;
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        if (inputs[i].selected && inputs[i].handle == handle) return &inputs[i];
    }
    return NULL;
}

static input_slot_t *slot_for_cid(bool classic, uint16_t cid) {
    if (!cid) return NULL;
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        if (inputs[i].selected && inputs[i].classic == classic && inputs[i].cid == cid) {
            return &inputs[i];
        }
    }
    return NULL;
}

static input_slot_t *slot_for_classic_address(const uint8_t *address) {
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        input_slot_t *slot = &inputs[i];
        if (slot->selected && slot->classic && !memcmp(slot->address, address, 6)) {
            return slot;
        }
    }
    return NULL;
}

static void release_input(input_slot_t *slot) {
    for (unsigned i = 0; i < slot->report_count; ++i) {
        memset(slot->reports[i].keys, 0, sizeof(slot->reports[i].keys));
        slot->reports[i].buttons = 0;
    }
    hub_router_input_disconnected(slot_index(slot));
}

static void reset_slot(input_slot_t *slot) {
    memset(slot, 0, sizeof(*slot));
    slot->handle = HCI_CON_HANDLE_INVALID;
    slot->last_auth_status = 0xff;
    slot->last_hid_status = 0xff;
    slot->last_disconnect_reason = 0xff;
}

static bool enrollment_permitted(void) {
    return hub_input_enrollment_permitted(enrollment_active,
        btstack_run_loop_get_time_ms(), enrollment_deadline, hub_security_input_pairing_blocked());
}

static uint8_t reserved_roles(const input_slot_t *except) {
    uint8_t roles = 0;
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        const input_slot_t *slot = &inputs[i];
        if (slot != except && slot->selected && (slot->stored || slot->state == INPUT_READY)) {
            roles |= slot->roles;
        }
    }
    return roles;
}

static unsigned saved_input_count(void) {
    unsigned count = 0;
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        if (inputs[i].selected && inputs[i].stored) ++count;
    }
    return count;
}

static bool any_closing(void) {
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        if (inputs[i].selected && inputs[i].state == INPUT_CLOSING) return true;
    }
    return false;
}

static int le_identity_index(uint8_t type, const uint8_t *address) {
    for (int i = 0; i < le_device_db_max_count(); ++i) {
        int db_type;
        bd_addr_t db_address;
        le_device_db_info(i, &db_type, db_address, NULL);
        if (db_type == type && !memcmp(address, db_address, 6)) return i;
    }
    return -1;
}

static bool le_has_key(int index) {
    if (index < 0 || index >= le_device_db_max_count()) return false;
    int key_size = 0;
    sm_key_t key;
    const uint8_t zero[16] = {0};
    le_device_db_encryption_get(index, NULL, NULL, key, &key_size, NULL, NULL, NULL);
    return key_size >= 7 && key_size <= 16 && memcmp(key, zero, sizeof(key)) != 0;
}

static bool target_has_bond(input_slot_t *slot) {
    if (slot->classic) {
        link_key_t key;
        link_key_type_t type;
        /* gap_get_link_key_for_bd_addr uses the Classic (ACL) key database. */
        return gap_get_link_key_for_bd_addr(slot->identity, key, &type);
    }
    return slot->identity_valid && le_has_key(le_identity_index(slot->identity_type, slot->identity));
}

static bool bond_capacity_available(input_slot_t *slot) {
    /* Both SDK TLV databases evict their oldest peer when full. Never let a
     * newly enrolled input evict a computer (or any unrelated peer). */
    if (!slot->classic) {
        if (le_identity_index(slot->address_type & 1u, slot->address) >= 0) return true;
        return le_device_db_count() < le_device_db_max_count();
    }
    btstack_link_key_iterator_t iterator;
    if (!gap_link_key_iterator_init(&iterator)) return false;
    bd_addr_t address;
    link_key_t key;
    link_key_type_t type;
    unsigned count = 0;
    bool known = false;
    while (gap_link_key_iterator_get_next(&iterator, address, key, &type)) {
        ++count;
        if (!memcmp(address, slot->address, 6)) known = true;
    }
    gap_link_key_iterator_done(&iterator);
    return known || count < NVM_NUM_LINK_KEYS;
}

static bool capture_identity(input_slot_t *slot, int index) {
    if (slot->classic) {
        if (!hub_input_stable_address(true, 0, slot->address)) return false;
        slot->identity_type = 0;
        memcpy(slot->identity, slot->address, 6);
        slot->identity_valid = true;
        return true;
    }
    if (index < 0 || index >= le_device_db_max_count()) return false;
    int type;
    bd_addr_t address;
    le_device_db_info(index, &type, address, NULL);
    if (!slot->stored && (type == 0 || type == 1)) {
        /* Keep the exact owned DB address for forget, even when not persistable. */
        slot->bond_address_valid = true;
        slot->bond_address_type = (uint8_t)type;
        memcpy(slot->bond_address, address, 6);
    }
    if (type < 0 || type > 1 || !hub_input_stable_address(false, (uint8_t)type, address)) {
        return false;
    }
    if (slot->stored && (slot->identity_type != type || memcmp(slot->identity, address, 6))) {
        printf("[input %u] bonded identity changed; refusing to overwrite target; input forget required\n",
               slot_index(slot) + 1);
        return false;
    }
    slot->identity_type = (uint8_t)type;
    memcpy(slot->identity, address, 6);
    slot->identity_valid = true;
    return true;
}

static bool save_target(input_slot_t *slot) {
    if (!capture_identity(slot, slot->classic ? -1 : sm_le_device_index(slot->handle)) ||
        !target_has_bond(slot)) {
        printf("[input %u] NOT saved: no stable bonded identity/key (an RPA is not an identity)\n",
               slot_index(slot) + 1);
        return false;
    }
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        input_slot_t *other = &inputs[i];
        if (other == slot || !other->selected || !other->identity_valid) continue;
        if (other->classic == slot->classic && other->identity_type == slot->identity_type &&
            !memcmp(other->identity, slot->identity, 6)) {
            printf("[input %u] identity already belongs to input %u; not replacing it\n",
                   slot_index(slot) + 1, i + 1);
            return false;
        }
    }
    if (slot->stored) return true;
    if (!input_tlv) {
        printf("[input %u] NOT saved: persistent TLV unavailable\n", slot_index(slot) + 1);
        return false;
    }
    hub_input_target_t target = {
        .classic = slot->classic, .type = slot->identity_type, .roles = slot->roles
    };
    memcpy(target.address, slot->identity, 6);
    uint8_t record[HUB_INPUT_TARGET_BYTES], readback[HUB_INPUT_TARGET_BYTES];
    hub_input_target_encode(&target, record);
    uint32_t tag = INPUT_TARGET_TAG + slot_index(slot);
    int status = input_tlv->store_tag(input_tlv_context, tag, record, sizeof(record));
    int length = input_tlv->get_tag(input_tlv_context, tag, readback, sizeof(readback));
    if (status || length != (int)sizeof(record) || memcmp(record, readback, sizeof(record))) {
        printf("[input %u] target write/readback FAILED: status=%d length=%d; enrollment stopped\n",
               slot_index(slot) + 1, status, length);
        slot->storage_error = true;
        corrupt_target[slot_index(slot)] = true;
        return false;
    }
    slot->stored = true;
    printf("[input %u] saved %s identity %s type=%u roles=%u (bond retained by SDK)\n",
           slot_index(slot) + 1, slot->classic ? "Classic" : "BLE",
           bd_addr_to_str(slot->identity), slot->identity_type, slot->roles);
    return true;
}

static void load_targets(void) {
    btstack_tlv_get_instance(&input_tlv, &input_tlv_context);
    if (!input_tlv) {
        printf("[input] persistent TLV unavailable; automatic enrollment disabled\n");
        return;
    }
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        uint8_t record[HUB_INPUT_TARGET_BYTES + 1u];
        int length = input_tlv->get_tag(input_tlv_context, INPUT_TARGET_TAG + i, record, sizeof(record));
        if (!length) continue;
        hub_input_target_t target;
        if (length < 0 || !hub_input_target_decode(record, (unsigned)length, &target)) {
            corrupt_target[i] = true;
            printf("[input %u] corrupt/unsupported target record (%d bytes); no automatic connection\n", i + 1, length);
            continue;
        }
        if (i && inputs[0].stored &&
            (((inputs[0].roles & target.roles)
              && (inputs[0].roles | target.roles) != HUB_INPUT_ROLE_BOTH) ||
             (inputs[0].classic == target.classic && inputs[0].identity_type == target.type &&
              !memcmp(inputs[0].identity, target.address, 6)))) {
            corrupt_target[i] = true;
            printf("[input %u] conflicting stored identity/roles; automatic connection blocked\n", i + 1);
            continue;
        }
        input_slot_t *slot = &inputs[i];
        slot->selected = true;
        slot->stored = true;
        slot->classic = target.classic;
        slot->roles = target.roles;
        slot->identity_valid = true;
        slot->identity_type = target.type;
        memcpy(slot->identity, target.address, 6);
        slot->address_type = target.type;
        memcpy(slot->address, target.address, 6);
        slot->ever_ready = true;
        slot->retry_allowed = true;
        slot->state = INPUT_RETRY;
        slot->deadline = btstack_run_loop_get_time_ms() + 500u + i * 250u;
        printf("[input %u] loaded %s target %s roles=%u; boot reconnect is bond-only\n",
               i + 1, target.classic ? "Classic" : "BLE", bd_addr_to_str(target.address), target.roles);
    }
}

static void close_input(input_slot_t *slot, uint8_t error, bool retry) {
    slot->last_error = error;
    slot->pairing_authorized = false;
    slot->retry_allowed = retry && !slot->open_test && (slot->ever_ready || slot->stored) &&
                          !slot->auth_failed && !slot->paused;
    if (slot->candidate && !slot->stored && !slot->storage_error) slot->remove = true;
    slot->state = INPUT_CLOSING;
    release_input(slot);
}

static void input_error(input_slot_t *slot, const char *operation, uint8_t error) {
    printf("[input %u] %s failed: status=0x%02x\n", slot_index(slot) + 1, operation, error);
    close_input(slot, error, slot->stored);
}

static input_report_t *find_report(input_slot_t *slot, uint8_t service, uint16_t id,
                                  bool create) {
    for (unsigned i = 0; i < slot->report_count; ++i) {
        input_report_t *report = &slot->reports[i];
        if (report->service == service && report->id == id) return report;
    }
    if (!create || slot->report_count == INPUT_REPORTS) return NULL;
    input_report_t *report = &slot->reports[slot->report_count++];
    memset(report, 0, sizeof(*report));
    report->service = service;
    report->id = id;
    return report;
}

/*
 * BTstack's compact parser does not implement global Push/Pop or keep a bit
 * cursor for revisited Report IDs. Reject those layouts rather than decoding
 * plausible but wrong keys. Descriptor copies also survive SDK storage moves.
 */
static bool save_descriptor(input_slot_t *slot, uint8_t service,
                            const uint8_t *data, uint16_t length) {
    if (!data || !length || service >= INPUT_SERVICES ||
        length > INPUT_DESCRIPTOR_BYTES - slot->descriptor_used) {
        printf("[input %u] missing/oversized descriptor: service=%u bytes=%u\n",
               slot_index(slot) + 1, service, length);
        return false;
    }
    uint16_t id = HID_REPORT_ID_UNDEFINED;
    uint32_t page = 0, local_usage = 0;
    uint32_t report_size = 0, report_count = 0;
    uint8_t application = 0, collection_depth = 0;
    uint8_t applications[16];
    bool seen_id[256] = {false};
    bool have_unnumbered_input = false;
    bool have_ids = false, ignored = false;
    bool local_axis = false;
    btstack_hid_descriptor_iterator_t iterator;
    btstack_hid_descriptor_iterator_init(&iterator, data, length);
    while (btstack_hid_descriptor_iterator_has_more(&iterator)) {
        const hid_descriptor_item_t *item = btstack_hid_descriptor_iterator_get_item(&iterator);
        uint32_t value = (uint32_t)item->item_value;
        if (item->item_type == Reserved) goto unsupported;
        if (item->item_type == Global) {
            switch (item->item_tag) {
                case UsagePage:
                    if (value > 0xffffu) goto unsupported;
                    page = value;
                    break;
                case ReportSize:
                    if (value > 32u) goto unsupported;
                    report_size = value;
                    break;
                case ReportCount:
                    if (value > 255u) goto unsupported;
                    report_count = value;
                    break;
                case ReportID:
                    if (!value || value > 255u || have_unnumbered_input) goto unsupported;
                    if (id != value && seen_id[value]) goto unsupported;
                    seen_id[value] = true;
                    id = (uint16_t)value;
                    have_ids = true;
                    break;
                case Push:
                case Pop:
                    goto unsupported;
                default:
                    if (item->item_tag > Pop) goto unsupported;
                    break;
            }
        } else if (item->item_type == Local) {
            if (item->item_tag == Usage || item->item_tag == UsageMinimum ||
                item->item_tag == UsageMaximum) {
                uint32_t usage = item->data_size > 2 ? value : (page << 16) | value;
                if (item->item_tag == Usage) local_usage = usage;
                if (usage == 0x00010030u || usage == 0x00010031u ||
                    usage == 0x00010038u) local_axis = true;
            } else if (item->item_tag == Delimiter) {
                goto unsupported;
            }
        } else if (item->item_type == Main) {
            switch (item->item_tag) {
                case Coll:
                    if (collection_depth == sizeof(applications)) goto unsupported;
                    applications[collection_depth++] = application;
                    if (value == 1) {
                        application = local_usage == 0x00010002u ? 2 :
                                      (local_usage == 0x00010006u ||
                                       local_usage == 0x00010007u) ? 1 : 0;
                    }
                    break;
                case EndColl:
                    if (!collection_depth) goto unsupported;
                    application = applications[--collection_depth];
                    break;
                case Input: {
                    if (!report_size || !report_count) goto unsupported;
                    if (id == HID_REPORT_ID_UNDEFINED) {
                        if (have_ids) goto unsupported;
                        have_unnumbered_input = true;
                    }
                    input_report_t *report = find_report(slot, service, id, true);
                    if (!report || report_size * report_count > 8192u - report->bits) {
                        goto unsupported;
                    }
                    report->bits += (uint16_t)(report_size * report_count);
                    if (!(value & 1u)) {
                        if ((page == HID_USAGE_PAGE_KEYBOARD && application != 1) ||
                            (local_axis && application != 2)) goto unsupported;
                        if (application == 1) report->keyboard = true;
                        if (application == 2) report->mouse = true;
                        if (!application) ignored = true;
                    }
                    break;
                }
                case Output:
                case Feature:
                    break;
                default:
                    goto unsupported;
            }
            local_usage = 0;
            local_axis = false;
        }
    }
    if (!btstack_hid_descriptor_iterator_valid(&iterator) || collection_depth) goto unsupported;

    bool keyboard_fields[INPUT_REPORTS] = {false};
    bool mouse_fields[INPUT_REPORTS] = {false};
    btstack_hid_usage_iterator_t usages;
    btstack_hid_usage_iterator_init(&usages, data, length, HID_REPORT_TYPE_INPUT);
    while (btstack_hid_usage_iterator_has_more(&usages)) {
        btstack_hid_usage_item_t item;
        btstack_hid_usage_iterator_get_item(&usages, &item);
        input_report_t *report = find_report(slot, service, item.report_id, false);
        if (!report || !item.size || item.size > 32 ||
            ((item.bit_pos & 7u) + item.size + 7u) / 8u > 4u ||
            (uint32_t)item.bit_pos + item.size > report->bits) goto unsupported;
        if (item.usage_page == HID_USAGE_PAGE_DESKTOP &&
            (item.usage == 0x30 || item.usage == 0x31 || item.usage == 0x38)) {
            if (!report->mouse || !(item.descriptor_item.item_value & 4) ||
                !(item.descriptor_item.item_value & 2) || item.global_logical_minimum >= 0) {
                printf("[input %u] unsupported non-mouse/absolute/unsigned axis: usage=0x%04x\n",
                       slot_index(slot) + 1, item.usage);
                return false;
            }
            mouse_fields[report - slot->reports] = true;
        }
        if (item.usage_page == HID_USAGE_PAGE_BUTTON && report->mouse) {
            if (!(item.descriptor_item.item_value & 2)) goto unsupported;
            if (item.usage >= 1 && item.usage <= 5) {
                mouse_fields[report - slot->reports] = true;
            } else {
                ignored = true;
            }
        }
        if (item.usage_page == HID_USAGE_PAGE_KEYBOARD) {
            if (!report->keyboard) goto unsupported;
            keyboard_fields[report - slot->reports] = true;
        }
    }
    for (unsigned i = 0; i < slot->report_count; ++i) {
        if (slot->reports[i].service != service) continue;
        slot->reports[i].keyboard = keyboard_fields[i];
        slot->reports[i].mouse = mouse_fields[i];
    }
    input_descriptor_t *descriptor = &slot->descriptors[service];
    descriptor->offset = slot->descriptor_used;
    descriptor->len = length;
    descriptor->report_ids = have_ids;
    memcpy(slot->descriptor + slot->descriptor_used, data, length);
    slot->descriptor_used += length;
    if (ignored) {
        printf("[input %u] media/vendor/non-keyboard-mouse collections are ignored\n", slot_index(slot) + 1);
    }
    return true;

unsupported:
    printf("[input %u] unsupported or malformed HID descriptor, service=%u\n",
           slot_index(slot) + 1, service);
    return false;
}

static bool descriptor_has_input(const input_slot_t *slot) {
    for (unsigned i = 0; i < slot->report_count; ++i) {
        if (slot->reports[i].keyboard || slot->reports[i].mouse) return true;
    }
    printf("[input %u] descriptor has no supported keyboard/relative mouse\n", slot_index(slot) + 1);
    return false;
}

static void input_ready(input_slot_t *slot) {
    if (slot->state == INPUT_READY) return;
    slot->hid_validated = true;
    if (slot->state == INPUT_CLOSING || slot->remove) return;
    if (slot->candidate && !enrollment_permitted()) {
        close_input(slot, ERROR_CODE_CONNECTION_TIMEOUT, false);
        return;
    }
    /* Encryption Change precedes LE key distribution; wait for SM completion. */
    if (!slot->open_test
        && (!slot->security_complete || !gap_encryption_key_size(slot->handle))) return;
    uint8_t actual_roles = 0;
    for (unsigned i = 0; i < slot->report_count; ++i) {
        if (slot->reports[i].keyboard) actual_roles |= HUB_INPUT_ROLE_KEYBOARD;
        if (slot->reports[i].mouse) actual_roles |= HUB_INPUT_ROLE_MOUSE;
    }
    if (slot->stored) {
        if ((actual_roles & slot->roles) != slot->roles) {
            input_error(slot, "stored HID role changed; input forget required",
                        ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
            return;
        }
    } else {
        slot->roles = hub_input_assign_roles(actual_roles, reserved_roles(slot));
        if (!slot->roles) {
            input_error(slot, "keyboard/mouse role already reserved by another input",
                        ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
            return;
        }
    }
    if (!slot->open_test && !save_target(slot)) {
        input_error(slot, "persistent bonded target validation", ERROR_CODE_AUTHENTICATION_FAILURE);
        if (slot->storage_error) hub_input_enroll_stop();
        return;
    }
    if (actual_roles == HUB_INPUT_ROLE_BOTH) {
        printf("[input %u] composite HID supplies reserved roles=%u; other stored roles stay untouched\n",
               slot_index(slot) + 1, slot->roles);
    }
    slot->state = INPUT_READY;
    slot->candidate = false;
    slot->ever_ready = true;
    slot->pairing_authorized = false;
    slot->retries = 0;
    slot->last_error = 0;
    if (establishing == (int)slot_index(slot)) establishing = -1;
    if (!slot->open_test || open_bridge_enabled) hub_router_input_connected(slot_index(slot));
    printf("[input %u] HID ready: %s handle=0x%04x cid=0x%04x descriptors=%u bytes\n",
           slot_index(slot) + 1, slot->classic ? "Classic" : "BLE",
           slot->handle, slot->cid, slot->descriptor_used);
    if (slot->open_test) {
        printf("[open-test] input %u HID ready, encryption-key=%u, no target saved, forwarding %s\n",
               slot_index(slot) + 1, gap_encryption_key_size(slot->handle),
               open_bridge_enabled ? "ON (explicit opt-in)" : "OFF");
    }
    if (enrollment_active && saved_input_count() == HUB_INPUT_COUNT
        && reserved_roles(NULL) == HUB_INPUT_ROLE_BOTH) {
        printf("[input] two devices with keyboard/mouse roles saved; enrollment complete\n");
        hub_input_enroll_stop();
    }
}

static int32_t saturating_add(int32_t a, int32_t b) {
    int64_t sum = (int64_t)a + b;
    if (sum > INT32_MAX) return INT32_MAX;
    if (sum < INT32_MIN) return INT32_MIN;
    return (int32_t)sum;
}

static void send_keyboard(input_slot_t *slot) {
    uint8_t keys[32] = {0};
    uint8_t boot[8] = {0};
    for (unsigned i = 0; i < slot->report_count; ++i) {
        for (unsigned j = 0; j < sizeof(keys); ++j) keys[j] |= slot->reports[i].keys[j];
    }
    boot[0] = keys[0xe0 / 8];
    unsigned count = 0;
    bool rollover = (keys[0] & 0x0eu) != 0;
    for (unsigned key = 4; key < 256; ++key) {
        if (key >= 0xe0 && key <= 0xe7) continue;
        if (!(keys[key / 8] & (1u << (key & 7)))) continue;
        if (count < 6) boot[2 + count] = (uint8_t)key;
        ++count;
    }
    if (count > 6 || rollover) memset(boot + 2, 1, 6);
    hub_router_keyboard(slot_index(slot), boot);
}

static void handle_report(input_slot_t *slot, uint8_t service,
                          const uint8_t *data, uint16_t length) {
    if (slot->state != INPUT_READY || slot->remove || service >= slot->service_count) return;
    if (slot->open_test) {
        if (!length) {
            printf("[open-test] empty input report ignored\n");
            return;
        }
        ++slot->test_reports;
        if (slot->test_reports == 1 || (slot->test_reports & 31u) == 0) {
            printf("[open-test] input %u reports=%" PRIu32 " bytes=%u encryption-key=%u; bridge=%u\n",
                   slot_index(slot) + 1, slot->test_reports, length,
                   gap_encryption_key_size(slot->handle), open_bridge_enabled);
        }
        if (!open_bridge_enabled) return;
    }
    const input_descriptor_t *descriptor = &slot->descriptors[service];
    input_report_t *report = NULL;
    if (!length) goto invalid;
    uint16_t id = descriptor->report_ids ? data[0] : HID_REPORT_ID_UNDEFINED;
    report = find_report(slot, service, id, false);
    if (!report || length != (report->bits + 7u) / 8u + (descriptor->report_ids ? 1u : 0u)) {
        goto invalid;
    }
    uint8_t keys[32] = {0}, buttons = report->buttons;
    bool keyboard_present = false, mouse_present = false;
    int32_t dx = 0, dy = 0, wheel = 0;
    btstack_hid_parser_t parser;
    btstack_hid_parser_init(&parser, slot->descriptor + descriptor->offset,
                            descriptor->len, HID_REPORT_TYPE_INPUT, data, length);
    while (btstack_hid_parser_has_more(&parser)) {
        uint16_t page, usage;
        int32_t value;
        btstack_hid_parser_get_field(&parser, &page, &usage, &value);
        if (page == HID_USAGE_PAGE_KEYBOARD && report->keyboard) {
            keyboard_present = true;
            if (value && usage && usage < 256) keys[usage / 8] |= (uint8_t)(1u << (usage & 7));
        } else if (page == 0x09 && report->mouse && usage >= 1 && usage <= 5) {
            uint8_t mask = (uint8_t)(1u << (usage - 1));
            buttons = value ? buttons | mask : buttons & (uint8_t)~mask;
            mouse_present = true;
        } else if (page == HID_USAGE_PAGE_DESKTOP && report->mouse) {
            switch (usage) {
                case 0x30: dx = saturating_add(dx, value); mouse_present = true; break;
                case 0x31: dy = saturating_add(dy, value); mouse_present = true; break;
                case 0x38: wheel = saturating_add(wheel, value); mouse_present = true; break;
                default: break;
            }
        }
    }
    if (keyboard_present && (slot->roles & HUB_INPUT_ROLE_KEYBOARD)) {
        memcpy(report->keys, keys, sizeof(keys));
        send_keyboard(slot);
    }
    if (mouse_present && (slot->roles & HUB_INPUT_ROLE_MOUSE)) {
        report->buttons = buttons;
        buttons = 0;
        for (unsigned i = 0; i < slot->report_count; ++i) buttons |= slot->reports[i].buttons;
        hub_router_mouse(slot_index(slot), buttons, dx, dy, wheel);
    }
    return;

invalid:
    if (!slot->warned_report) {
        printf("[input %u] malformed/unknown HID report: service=%u bytes=%u\n",
               slot_index(slot) + 1, service, length);
        slot->warned_report = true;
    }
    if (report) {
        memset(report->keys, 0, sizeof(report->keys));
        report->buttons = 0;
        if (report->keyboard) send_keyboard(slot);
        if (report->mouse) {
            uint8_t merged = 0;
            for (unsigned i = 0; i < slot->report_count; ++i) merged |= slot->reports[i].buttons;
            hub_router_mouse(slot_index(slot), merged, 0, 0, 0);
        }
    }
}

static void discover_le(input_slot_t *slot) {
    if (slot->classic || slot->state != INPUT_SECURITY || slot->remove ||
        !gap_encryption_key_size(slot->handle)) return;
    slot->state = INPUT_DISCOVERING;
    slot->deadline = btstack_run_loop_get_time_ms() + INPUT_DISCOVERY_MS;
    uint8_t status = hids_host_connect(slot->handle, input_hids_event,
                                       HID_PROTOCOL_MODE_REPORT, &slot->cid);
    if (status) {
        slot->cid = 0;
        input_error(slot, "HIDS discovery start", status);
    }
}

static bool start_connection(input_slot_t *slot) {
    if (hub_security_input_pairing_blocked()) {
        printf("[input %u] connection deferred until PC pairing policy is idle\n",
               slot_index(slot) + 1);
        return false;
    }
    slot->last_auth_status = 0xff;
    slot->last_hid_status = 0xff;
    slot->last_disconnect_reason = 0xff;
    if (slot->pairing_authorized && !bond_capacity_available(slot)) {
        input_error(slot, "bond database full/unavailable; refusing to evict unrelated keys",
                    ERROR_CODE_MEMORY_CAPACITY_EXCEEDED);
        return false;
    }
    if (!slot->pairing_authorized && !slot->open_test) {
        if (!slot->identity_valid || !target_has_bond(slot)) {
            slot->auth_failed = true;
            slot->state = INPUT_FAILED;
            printf("[input %u] saved bond missing; no new pairing authorized; use input forget/enroll or explicit connect\n",
                   slot_index(slot) + 1);
            return false;
        }
        memcpy(slot->address, slot->identity, 6);
        slot->address_type = slot->identity_type;
        if (!slot->classic) {
            int index = le_identity_index(slot->identity_type, slot->identity);
            sm_key_t irk;
            const uint8_t zero[16] = {0};
            le_device_db_info(index, NULL, NULL, irk);
            if (memcmp(irk, zero, sizeof(irk))) {
                if (!resolving_list_supported) {
                    slot->state = INPUT_FAILED;
                    printf("[input %u] privacy reconnect unavailable: controller resolving list unsupported\n",
                           slot_index(slot) + 1);
                    return false;
                }
                /* HCI Create Connection must use the identity address types 2/3. */
                slot->address_type = slot->identity_type + 2u;
            }
        }
    }
    slot->state = INPUT_CONNECTING;
    slot->disconnect_sent = false;
    slot->descriptor_used = 0;
    slot->service_count = 0;
    slot->report_count = 0;
    slot->warned_report = false;
    slot->protocol_pending = false;
    slot->hid_validated = false;
    slot->security_complete = false;
    memset(slot->descriptors, 0, sizeof(slot->descriptors));
    establishing = (int)slot_index(slot);
    slot->deadline = btstack_run_loop_get_time_ms() +
                     ((slot->classic && slot->pairing_authorized) ? HUB_PAIRING_WINDOW_MS : INPUT_CONNECT_MS);
    uint8_t status = slot->classic ?
        hid_host_connect(slot->address, HID_PROTOCOL_MODE_REPORT, &slot->cid) :
        gap_connect(slot->address, (bd_addr_type_t)slot->address_type);
    if (status) {
        slot->cid = 0;
        printf("[input %u] %s connect rejected: status=0x%02x\n",
               slot_index(slot) + 1, slot->classic ? "Classic" : "BLE", status);
        close_input(slot, status, slot->retry_allowed);
        establishing = -1;
        return false;
    }
    printf("[input %u] connecting %s %s (%s), pairing %s\n", slot_index(slot) + 1,
           slot->classic ? "Classic" : "BLE", bd_addr_to_str(slot->address),
           (slot->address_type & 1u) ? "random/identity" : "public/identity",
           slot->open_test ? "NOT REQUIRED (Level 0 test)"
                          : slot->pairing_authorized ? "authorized once" : "bond-only");
    return true;
}

static bool delete_target(input_slot_t *slot) {
    unsigned index = slot_index(slot);
    if (!input_tlv) return false;
    /* Delete the app target first: a power loss must not reauthorize pairing. */
    input_tlv->delete_tag(input_tlv_context, INPUT_TARGET_TAG + index);
    if (input_tlv->get_tag(input_tlv_context, INPUT_TARGET_TAG + index, NULL, 0) != 0) {
        printf("[input %u] FORGET FAILED: target deletion did not persist; bonds retained\n", index + 1);
        return false;
    }
    if (slot->classic) {
        gap_drop_link_key_for_bd_addr(slot->identity_valid ? slot->identity : slot->address);
        link_key_t key;
        link_key_type_t type;
        if (gap_get_link_key_for_bd_addr(slot->identity_valid ? slot->identity : slot->address, key, &type)) {
            printf("[input %u] FORGET FAILED: Classic key deletion did not persist\n", index + 1);
            return false;
        }
    } else if (slot->identity_valid || slot->bond_address_valid) {
        uint8_t type = slot->identity_valid ? slot->identity_type : slot->bond_address_type;
        uint8_t *address = slot->identity_valid ? slot->identity : slot->bond_address;
        gap_delete_bonding((bd_addr_type_t)type, address);
        if (le_identity_index(type, address) >= 0) {
            printf("[input %u] FORGET FAILED: LE bond deletion did not persist\n", index + 1);
            return false;
        }
    }
    corrupt_target[index] = false;
    printf("[input %u] target and its %s bond forgotten; unrelated keys untouched\n",
           index + 1, slot->classic ? "Classic" : "BLE");
    return true;
}

static void finish_close(input_slot_t *slot, uint32_t now) {
    if (establishing == (int)slot_index(slot)) establishing = -1;
    if (slot->forget) {
        forgetting &= ~(1u << slot_index(slot));
        if (!delete_target(slot)) {
            enrollment_pending = false;
            slot->forget = false;
            slot->remove = false;
            slot->paused = true;
            slot->state = INPUT_FAILED;
            return;
        }
        reset_slot(slot);
    } else if (slot->remove) {
        printf("[input %u] removed\n", slot_index(slot) + 1);
        reset_slot(slot);
    } else if (slot->retry_allowed && !slot->auth_failed) {
        slot->state = INPUT_RETRY;
        slot->deadline = now + (slot->retries < INPUT_RETRIES ?
                         (1000u << slot->retries) : INPUT_SLOW_RETRY_MS);
        printf("[input %u] bond-only reconnect in %" PRIu32 " ms%s\n",
               slot_index(slot) + 1, slot->deadline - now,
               slot->retries < INPUT_RETRIES ? "" : " (ongoing slow retry)");
    } else {
        slot->state = INPUT_FAILED;
        printf("[input %u] stopped; saved identity/keys retained; explicit connect or input forget to recover\n",
               slot_index(slot) + 1);
    }
}

static void input_tick(btstack_timer_source_t *timer) {
    uint32_t now = btstack_run_loop_get_time_ms();
    if (enrollment_active && !enrollment_permitted()) {
        printf("[input] enrollment expired or PC keyboard security became busy\n");
        hub_input_enroll_stop();
    }
    if ((scanning == SCAN_CLASSIC || scanning == SCAN_LE) && expired(now, scan_deadline)) {
        hub_input_scan_stop();
    }
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        input_slot_t *slot = &inputs[i];
        if (!slot->selected) continue;
        if (slot->hid_validated && slot->state == INPUT_DISCOVERING) input_ready(slot);
        if ((slot->state == INPUT_CONNECTING || slot->state == INPUT_SECURITY ||
             slot->state == INPUT_DISCOVERING) && expired(now, slot->deadline)) {
            printf("[input %u] establishment timeout\n", i + 1);
            close_input(slot, ERROR_CODE_CONNECTION_TIMEOUT, slot->retry_allowed);
        }
        if (slot->state == INPUT_CLOSING) {
            if (slot->handle != HCI_CON_HANDLE_INVALID) {
                if (!slot->disconnect_sent) {
                    uint8_t status = gap_disconnect(slot->handle);
                    printf("[input %u] disconnect handle=0x%04x request=0x%02x\n",
                           i + 1, slot->handle, status);
                    slot->disconnect_sent = !status;
                }
            } else if (!slot->classic && establishing == (int)i) {
                if (!slot->disconnect_sent) {
                    uint8_t status = gap_connect_cancel();
                    printf("[input %u] LE cancel request=0x%02x\n", i + 1, status);
                    slot->disconnect_sent = !status;
                }
            } else if (!slot->cid) {
                finish_close(slot, now);
            }
            /*
             * Classic SDP has no cancel API. Retain its CID/target until its
             * completion callback; disconnect an arriving ACL before use.
             */
        }
        if (slot->state == INPUT_RETRY && expired(now, slot->deadline) &&
            establishing < 0 && scanning == SCAN_OFF && !hub_security_input_pairing_blocked() &&
            !(HUB_CLASSIC_OPEN_TEST && slot->classic) &&
            !enrollment_active && !enrollment_pending && !forgetting && !any_closing()) {
            if (slot->retries < INPUT_RETRIES) ++slot->retries;
            start_connection(slot);
        }
    }
    enrollment_tick(now);
    btstack_run_loop_set_timer(timer, 100);
    btstack_run_loop_add_timer(timer);
}

static void input_hid_event(uint8_t packet_type, uint16_t channel,
                            uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type != HCI_EVENT_PACKET || size < 5 || packet[0] != HCI_EVENT_HID_META) return;
    uint8_t event = hci_event_hid_meta_get_subevent_code(packet);
    uint16_t cid = little_endian_read_16(packet, 3);
    input_slot_t *slot = slot_for_cid(true, cid);
    if (event == HID_SUBEVENT_INCOMING_CONNECTION) {
        if (size < 14) return;
        bd_addr_t address;
        hid_subevent_incoming_connection_get_address(packet, address);
        slot = slot_for_classic_address(address);
        uint8_t status = hid_subevent_incoming_connection_get_status(packet);
        if (status) {
            printf("[input] incoming Classic HID status=0x%02x\n", status);
            return;
        }
        if (!slot || slot->remove || slot->auth_failed || slot->paused || slot->cid ||
            (slot->state != INPUT_RETRY && slot->state != INPUT_FAILED) ||
            establishing >= 0 || scanning != SCAN_OFF || hub_security_input_pairing_blocked() ||
            (!slot->open_test && !target_has_bond(slot))) {
            status = hid_host_decline_connection(cid);
            printf("[input] declined unselected/busy Classic HID %s status=0x%02x\n",
                   bd_addr_to_str(address), status);
            return;
        }
        slot->cid = cid;
        slot->handle = hid_subevent_incoming_connection_get_handle(packet);
        slot->state = INPUT_CONNECTING;
        slot->disconnect_sent = false;
        slot->protocol_pending = true;
        slot->report_count = 0;
        slot->descriptor_used = 0;
        slot->hid_validated = false;
        slot->security_complete = gap_encryption_key_size(slot->handle) != 0;
        establishing = (int)slot_index(slot);
        slot->deadline = btstack_run_loop_get_time_ms() + INPUT_DISCOVERY_MS;
        status = hid_host_accept_connection(cid, HID_PROTOCOL_MODE_REPORT);
        if (status) input_error(slot, "accept Classic HID", status);
        return;
    }
    if (!slot) return;
    switch (event) {
        case HID_SUBEVENT_CONNECTION_OPENED: {
            if (size < 15) return;
            uint8_t status = hid_subevent_connection_opened_get_status(packet);
            slot->last_hid_status = status;
            printf("[input %u] Classic HID open status=0x%02x cid=0x%04x\n",
                   slot_index(slot) + 1, status, cid);
            if (status) {
                slot->cid = 0;
                close_input(slot, status, slot->retry_allowed);
                if (status == ERROR_CODE_AUTHENTICATION_FAILURE ||
                    status == ERROR_CODE_PIN_OR_KEY_MISSING) slot->auth_failed = true;
            } else {
                slot->handle = hid_subevent_connection_opened_get_con_handle(packet);
                if (slot->state != INPUT_CLOSING) {
                    slot->state = INPUT_DISCOVERING;
                    slot->deadline = btstack_run_loop_get_time_ms() + INPUT_DISCOVERY_MS;
                }
            }
            break;
        }
        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE: {
            if (size < 6 || slot->state == INPUT_CLOSING) break;
            uint8_t status = hid_subevent_descriptor_available_get_status(packet);
            if (status) {
                input_error(slot, "Classic descriptor", status);
                break;
            }
            slot->service_count = 1;
            if (!save_descriptor(slot, 0, hid_descriptor_storage_get_descriptor_data(cid),
                                 hid_descriptor_storage_get_descriptor_len(cid)) ||
                !descriptor_has_input(slot)) {
                input_error(slot, "Classic descriptor validation", ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                break;
            }
            if (!slot->protocol_pending) input_ready(slot);
            break;
        }
        case HID_SUBEVENT_REPORT: {
            if (size < 7) break;
            uint16_t length = hid_subevent_report_get_report_len(packet);
            const uint8_t *report = hid_subevent_report_get_report(packet);
            if (length > size - 7u || !length) break;
            // This HIDP callback carries the transaction/type byte, not a Report ID.
            if (report[0] != 0xa1) break;
            handle_report(slot, 0, report + 1, length - 1u);
            break;
        }
        case HID_SUBEVENT_SET_PROTOCOL_RESPONSE:
            if (size < 7) break;
            if (hid_subevent_set_protocol_response_get_handshake_status(packet) ||
                hid_subevent_set_protocol_response_get_protocol_mode(packet) != HID_PROTOCOL_MODE_REPORT) {
                printf("[input %u] unsupported Classic protocol: handshake=0x%02x mode=%u\n",
                       slot_index(slot) + 1, hid_subevent_set_protocol_response_get_handshake_status(packet),
                       hid_subevent_set_protocol_response_get_protocol_mode(packet));
                input_error(slot, "report protocol", ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
            } else {
                slot->protocol_pending = false;
                if (slot->state == INPUT_DISCOVERING && slot->descriptor_used) input_ready(slot);
            }
            break;
        case HID_SUBEVENT_VIRTUAL_CABLE_UNPLUG:
            slot->auth_failed = true;
            close_input(slot, 0, false);
            break;
        case HID_SUBEVENT_CONNECTION_CLOSED:
            slot->cid = 0;
            if (slot->state != INPUT_CLOSING) close_input(slot, 0, true);
            else release_input(slot);
            break;
        default:
            break;
    }
}

static void input_hids_event(uint8_t packet_type, uint16_t channel,
                             uint8_t *packet, uint16_t size) {
    (void)channel;
    // At this revision notifications use HCI_EVENT_GATTSERVICE_META as packet_type.
    if ((packet_type != HCI_EVENT_PACKET && packet_type != HCI_EVENT_GATTSERVICE_META) ||
        size < 5 || packet[0] != HCI_EVENT_GATTSERVICE_META) return;
    input_slot_t *slot = slot_for_cid(false, little_endian_read_16(packet, 3));
    if (!slot) return;
    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED: {
            if (size < 8) return;
            uint8_t status = gattservice_subevent_hid_service_connected_get_status(packet);
            slot->last_hid_status = status;
            printf("[input %u] BLE HIDS discovery status=0x%02x\n", slot_index(slot) + 1, status);
            if (status) {
                slot->cid = 0; // HIDS host frees its context after this callback.
                input_error(slot, "BLE HIDS discovery", status);
                break;
            }
            if (slot->state == INPUT_CLOSING) break;
            slot->service_count = gattservice_subevent_hid_service_connected_get_num_instances(packet);
            if (!slot->service_count || slot->service_count > INPUT_SERVICES ||
                gattservice_subevent_hid_service_connected_get_protocol_mode(packet) != HID_PROTOCOL_MODE_REPORT) {
                input_error(slot, "BLE HIDS services/protocol", ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                break;
            }
            bool valid = true;
            for (uint8_t service = 0; service < slot->service_count; ++service) {
                uint16_t length = hids_host_descriptor_storage_get_descriptor_len(slot->cid, service);
                const uint8_t *data = hids_host_descriptor_storage_get_descriptor_data(slot->cid, service);
                if (!save_descriptor(slot, service, data, length)) {
                    valid = false;
                    break;
                }
            }
            if (!valid || !descriptor_has_input(slot)) {
                input_error(slot, "BLE descriptor validation", ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                break;
            }
            input_ready(slot);
            break;
        }
        case GATTSERVICE_SUBEVENT_HID_REPORT: {
            if (size < 9 || slot->state != INPUT_READY) break;
            uint8_t service = gattservice_subevent_hid_report_get_service_index(packet);
            uint8_t id = gattservice_subevent_hid_report_get_report_id(packet);
            uint16_t length = gattservice_subevent_hid_report_get_report_len(packet);
            const uint8_t *report = gattservice_subevent_hid_report_get_report(packet);
            if (service >= slot->service_count || length > size - 9u || !length || report[0] != id) {
                release_input(slot);
                break;
            }
            /*
             * hids_host.c inserts the Report Reference ID into EVERY callback.
             * Keep it for numbered maps; remove only its synthetic zero for a
             * map with no Report IDs. Never prepend a second ID.
             */
            if (!slot->descriptors[service].report_ids) {
                if (id) {
                    input_error(slot, "BLE Report Reference/Map mismatch",
                                ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                    break;
                }
                ++report;
                --length;
            }
            handle_report(slot, service, report, length);
            break;
        }
        case GATTSERVICE_SUBEVENT_HID_SERVICE_DISCONNECTED:
            /*
             * The pinned HIDS host compacts shared maps when a client goes
             * away. Its multi-service offset fixup uses the departing client's
             * service count. Cached ready peers are safe; abort an unfinished
             * discovery rather than accepting a potentially shifted map.
             */
            for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
                input_slot_t *other = &inputs[i];
                if (other != slot && other->selected && !other->classic &&
                    other->state == INPUT_DISCOVERING) {
                    input_error(other, "BLE descriptor storage changed during discovery; retry connect",
                                ERROR_CODE_COMMAND_DISALLOWED);
                }
            }
            slot->cid = 0;
            if (slot->state != INPUT_CLOSING) close_input(slot, 0, true);
            else release_input(slot);
            break;
        default:
            break;
    }
}

static void print_name(const uint8_t *name, unsigned length) {
    for (unsigned i = 0; i < length && i < 100; ++i) {
        uint8_t c = name[i];
        putchar(c >= 32 && c < 127 ? c : '.');
    }
}

static void offer_candidate(bool classic, uint8_t type, const uint8_t *address) {
    if (!enrollment_permitted() || pending_candidate >= 0 || establishing >= 0 ||
        candidate_count == INPUT_CANDIDATES || type > 1 ||
        !hub_input_stable_address(true, 0, address)) return;
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        const input_slot_t *slot = &inputs[i];
        if (!slot->selected || slot->classic != classic) continue;
        if (((slot->address_type & 1u) == type && !memcmp(slot->address, address, 6)) ||
            (slot->identity_valid && slot->identity_type == type && !memcmp(slot->identity, address, 6))) return;
    }
    for (unsigned i = 0; i < candidate_count; ++i) {
        if (candidates[i].classic == classic && candidates[i].type == type &&
            !memcmp(candidates[i].address, address, 6)) return;
    }
    input_candidate_t *candidate = &candidates[candidate_count];
    candidate->classic = classic;
    candidate->type = type;
    memcpy(candidate->address, address, 6);
    pending_candidate = (int)candidate_count++;
    printf("[input] enrollment candidate %s %s; stopping scan before connecting\n",
           classic ? "Classic" : "BLE", bd_addr_to_str(candidate->address));
    hub_input_scan_stop();
}

static void offer_ble_advertisement(uint8_t event_type, uint8_t type,
                                   const bd_addr_t address, const uint8_t *data,
                                   unsigned length) {
    if (!enrollment_permitted() || type > 1) return;
    input_scan_peer_t *peer = NULL;
    for (unsigned i = 0; i < INPUT_SCAN_PEERS; ++i) {
        if (scan_peers[i].used && scan_peers[i].type == type
            && memcmp(scan_peers[i].address, address, 6) == 0) {
            peer = &scan_peers[i];
            break;
        }
    }
    uint32_t now = btstack_run_loop_get_time_ms();
    if (event_type == 0) {
        if (!peer) {
            peer = &scan_peers[next_scan_peer];
            next_scan_peer = (next_scan_peer + 1u) % INPUT_SCAN_PEERS;
        }
        peer->used = true;
        peer->type = type;
        memcpy(peer->address, address, 6);
        peer->seen = now;
        peer->eligible = hub_input_ble_discoverable(data, length);
    } else if (event_type != 4) {
        /* Directed, scannable-only, and nonconnectable beacons are not enrollment. */
        if (peer) peer->eligible = false;
        return;
    }
    if (peer && peer->eligible && (uint32_t)(now - peer->seen) <= 2000u
        && hub_input_ble_hid_advertisement(data, length)) {
        offer_candidate(false, type, address);
    }
}

static void input_hci_event(uint8_t packet_type, uint16_t channel,
                            uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type != HCI_EVENT_PACKET || size < 2) return;
    input_slot_t *slot;
    bd_addr_t address;
    uint8_t status;
    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_COMMAND_STATUS: {
            if (size < 6) break;
            uint16_t opcode = hci_event_command_status_get_command_opcode(packet);
            status = hci_event_command_status_get_status(packet);
            if ((opcode == HCI_OPCODE_HCI_LE_CREATE_CONNECTION ||
                 opcode == HCI_OPCODE_HCI_LE_EXTENDED_CREATE_CONNECTION) && status &&
                establishing >= 0 && !inputs[establishing].classic &&
                inputs[establishing].handle == HCI_CON_HANDLE_INVALID) {
                // A rejected command does not produce LE Connection Complete.
                slot = &inputs[establishing];
                printf("[input %u] LE create command=0x%04x failed status=0x%02x\n",
                       slot_index(slot) + 1, opcode, status);
                establishing = -1;
                close_input(slot, status, slot->retry_allowed);
                break;
            }
            if (opcode != HCI_OPCODE_HCI_INQUIRY ||
                (scanning != SCAN_CLASSIC && scanning != SCAN_CLASSIC_STOPPING)) break;
            if (status) {
                printf("[scan Classic] inquiry command failed status=0x%02x\n", status);
                scanning = SCAN_OFF;
                classic_inquiry_active = false;
            } else {
                classic_inquiry_active = true;
                if (scanning == SCAN_CLASSIC_STOPPING) {
                    int result = gap_inquiry_stop();
                    printf("[scan Classic] deferred stop status=0x%02x\n", result);
                }
            }
            break;
        }
        case GAP_EVENT_INQUIRY_RESULT:
            if (scanning != SCAN_CLASSIC || size < 27) break;
            gap_event_inquiry_result_get_bd_addr(packet, address);
            printf("[scan Classic] %s", bd_addr_to_str(address));
            if (gap_event_inquiry_result_get_rssi_available(packet)) {
                printf(" RSSI=%d", gap_event_inquiry_result_get_rssi(packet));
            }
            if (gap_event_inquiry_result_get_name_available(packet)) {
                uint8_t length = gap_event_inquiry_result_get_name_len(packet);
                const uint8_t *name = gap_event_inquiry_result_get_name(packet);
                if ((size_t)(name - packet) + length <= size) {
                    printf(" name=");
                    print_name(name, length);
                }
            }
            printf("\n");
            if (hub_input_assign_roles(
                    hub_input_classic_roles(gap_event_inquiry_result_get_class_of_device(packet)),
                    reserved_roles(NULL))) {
                offer_candidate(true, 0, address);
            }
            break;
        case GAP_EVENT_INQUIRY_COMPLETE:
            if (size < 3) break;
            if (scanning == SCAN_CLASSIC || scanning == SCAN_CLASSIC_STOPPING) {
                printf("[scan Classic] finished status=0x%02x\n", gap_event_inquiry_complete_get_status(packet));
                scanning = SCAN_OFF;
                classic_inquiry_active = false;
            }
            break;
        case GAP_EVENT_ADVERTISING_REPORT: {
            if (scanning != SCAN_LE || size < 12) break;
            gap_event_advertising_report_get_address(packet, address);
            uint8_t type = gap_event_advertising_report_get_address_type(packet);
            uint8_t length = gap_event_advertising_report_get_data_length(packet);
            const uint8_t *data = gap_event_advertising_report_get_data(packet);
            if ((size_t)(data - packet) + length > size) break;
            printf("[scan BLE] %s type=%s(%u) RSSI=%d", bd_addr_to_str(address),
                   type == 0 ? "public" : type == 1 ? "random" : "other", type,
                   gap_event_advertising_report_get_rssi(packet));
            for (unsigned pos = 0; pos < length;) {
                unsigned field = data[pos];
                if (!field || pos + field + 1 > length) break;
                if (field >= 2 && (data[pos + 1] == 8 || data[pos + 1] == 9)) {
                    printf(" name=");
                    print_name(data + pos + 2, field - 1);
                    break;
                }
                pos += field + 1;
            }
            printf("\n");
            offer_ble_advertisement(
                gap_event_advertising_report_get_advertising_event_type(packet),
                type, address, data, length);
            break;
        }
        case HCI_EVENT_CONNECTION_COMPLETE:
            if (size < 13) break;
            hci_event_connection_complete_get_bd_addr(packet, address);
            slot = slot_for_classic_address(address);
            if (!slot) break;
            status = hci_event_connection_complete_get_status(packet);
            printf("[input %u] Classic ACL status=0x%02x\n", slot_index(slot) + 1, status);
            if (!status) {
                slot->handle = hci_event_connection_complete_get_connection_handle(packet);
                slot->disconnect_sent = false;
                if (slot->remove || slot->paused || slot->auth_failed || slot->state == INPUT_CLOSING) {
                    close_input(slot, 0, false);
                }
            } else {
                slot->last_error = status;
            }
            break;
        case HCI_EVENT_META_GAP: {
            if (size < 14 || hci_event_gap_meta_get_subevent_code(packet) != GAP_SUBEVENT_LE_CONNECTION_COMPLETE ||
                establishing < 0) break;
            slot = &inputs[establishing];
            if (slot->classic || slot->handle != HCI_CON_HANDLE_INVALID ||
                (slot->state != INPUT_CONNECTING && slot->state != INPUT_CLOSING)) break;
            status = gap_subevent_le_connection_complete_get_status(packet);
            // Advertising timeout belongs to an LE peripheral, not our initiator.
            if (status == 0x3c || gap_subevent_le_connection_complete_get_role(packet) != HCI_ROLE_MASTER) break;
            gap_subevent_le_connection_complete_get_peer_address(packet, address);
            const uint8_t zero[6] = {0};
            uint8_t peer_type = gap_subevent_le_connection_complete_get_peer_address_type(packet);
            bool address_matches = !memcmp(slot->address, address, 6) &&
                (peer_type & 1u) == (slot->address_type & 1u);
            if (!address_matches && slot->identity_valid) {
                address_matches = !memcmp(slot->identity, address, 6) &&
                                  (peer_type & 1u) == slot->identity_type;
            }
            if (!address_matches && !status && size >= 26 && slot->address_type == 1) {
                bd_addr_t peer_rpa;
                gap_subevent_le_connection_complete_get_peer_resolvable_private_address(packet, peer_rpa);
                address_matches = !memcmp(slot->address, peer_rpa, 6);
            }
            // Failed initiator events may have no valid address/handle.
            if (!address_matches && (!status || memcmp(address, zero, 6))) break;
            printf("[input %u] LE central connection status=0x%02x\n", slot_index(slot) + 1, status);
            if (status) {
                establishing = -1;
                slot->disconnect_sent = false;
                close_input(slot, status, slot->retry_allowed);
                break;
            }
            slot->handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
            slot->disconnect_sent = false;
            if (slot->state == INPUT_CLOSING || slot->remove) break;
            slot->state = INPUT_SECURITY;
            slot->deadline = btstack_run_loop_get_time_ms() + HUB_PAIRING_WINDOW_MS;
            if (hub_security_input_pairing_blocked()) {
                input_error(slot, "PC keyboard pairing blocks input authentication", ERROR_CODE_COMMAND_DISALLOWED);
                break;
            }
            if (gap_encryption_key_size(slot->handle)) {
                slot->security_complete = true;
                discover_le(slot);
            } else {
                /*
                 * This also re-encrypts an existing bond. Main owns all pairing
                 * confirmations and rejects a fresh pairing on bond-only retry.
                 */
                sm_request_pairing(slot->handle);
            }
            break;
        }
        case HCI_EVENT_AUTHENTICATION_COMPLETE:
            if (size < 5) break;
            slot = slot_for_handle(hci_event_authentication_complete_get_connection_handle(packet));
            if (!slot || !slot->classic) break;
            status = hci_event_authentication_complete_get_status(packet);
            slot->last_auth_status = status;
            printf("[input %u] Classic authentication status=0x%02x\n", slot_index(slot) + 1, status);
            slot->pairing_authorized = false;
            if (status) {
                slot->auth_failed = true;
                input_error(slot, "Classic authentication", status);
            } else {
                slot->security_complete = true;
            }
            break;
        case HCI_EVENT_ENCRYPTION_CHANGE:
        case HCI_EVENT_ENCRYPTION_CHANGE_V2:
            if (size < 6) break;
            slot = slot_for_handle(hci_event_encryption_change_get_connection_handle(packet));
            if (!slot) break;
            status = hci_event_encryption_change_get_status(packet);
            if (status || !hci_event_encryption_change_get_encryption_enabled(packet)) {
                slot->auth_failed = true;
                input_error(slot, "link encryption", status ? status : ERROR_CODE_AUTHENTICATION_FAILURE);
            } else {
                if (slot->classic) slot->security_complete = true;
                else discover_le(slot);
            }
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            if (size < 6) break;
            slot = slot_for_handle(hci_event_disconnection_complete_get_connection_handle(packet));
            if (!slot) break;
            status = hci_event_disconnection_complete_get_status(packet);
            printf("[input %u] HCI disconnected status=0x%02x reason=0x%02x\n",
                   slot_index(slot) + 1, status, hci_event_disconnection_complete_get_reason(packet));
            if (status) {
                slot->disconnect_sent = false;
                break;
            }
            slot->last_disconnect_reason = hci_event_disconnection_complete_get_reason(packet);
            slot->handle = HCI_CON_HANDLE_INVALID;
            slot->disconnect_sent = false;
            if (!slot->classic && establishing == (int)slot_index(slot)) establishing = -1;
            if (slot->state != INPUT_CLOSING) {
                close_input(slot, hci_event_disconnection_complete_get_reason(packet), true);
            } else {
                release_input(slot);
            }
            break;
        default:
            break;
    }
}

static void input_sm_event(uint8_t packet_type, uint16_t channel,
                           uint8_t *packet, uint16_t size) {
    (void)channel;
    if (packet_type != HCI_EVENT_PACKET || size < 4) return;
    input_slot_t *slot;
    uint8_t status;
    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_IDENTITY_CREATED:
        case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED: {
            /* Both events have the same handle/address/identity/index layout. */
            if (size < 20) return;
            slot = slot_for_handle(sm_event_identity_created_get_handle(packet));
            if (!slot || slot->classic) return;
            int index = sm_event_identity_created_get_index(packet);
            if (capture_identity(slot, index) && slot->candidate) {
                for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
                    input_slot_t *other = &inputs[i];
                    if (other == slot || !other->stored || other->classic ||
                        other->identity_type != slot->identity_type ||
                        memcmp(other->identity, slot->identity, 6)) continue;
                    input_error(slot, "resolved identity already reserved by another input",
                                ERROR_CODE_COMMAND_DISALLOWED);
                    break;
                }
            }
            break;
        }
        case SM_EVENT_PAIRING_COMPLETE:
            slot = slot_for_handle(sm_event_pairing_complete_get_handle(packet));
            if (!slot || slot->classic || size < 14) return;
            status = sm_event_pairing_complete_get_status(packet);
            slot->last_auth_status = status;
            printf("[input %u] LE pairing status=0x%02x reason=0x%02x\n",
                   slot_index(slot) + 1, status, sm_event_pairing_complete_get_reason(packet));
            slot->pairing_authorized = false;
            if (status) {
                slot->auth_failed = true;
                input_error(slot, "LE pairing", status);
            } else {
                slot->security_complete = true;
                discover_le(slot);
            }
            break;
        case SM_EVENT_REENCRYPTION_COMPLETE:
            slot = slot_for_handle(sm_event_reencryption_complete_get_handle(packet));
            if (!slot || slot->classic || size < 12) return;
            status = sm_event_reencryption_complete_get_status(packet);
            slot->last_auth_status = status;
            printf("[input %u] LE bond re-encryption status=0x%02x\n", slot_index(slot) + 1, status);
            if (status) {
                // Do not delete bonds or trigger endless pairing after key loss.
                slot->auth_failed = true;
                input_error(slot, "LE bond re-encryption (reauthorize explicitly)", status);
            } else {
                slot->pairing_authorized = false;
                slot->security_complete = true;
                discover_le(slot);
            }
            break;
        default:
            break;
    }
}

void hub_input_init(void) {
    open_bridge_enabled = false;
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) reset_slot(&inputs[i]);
    hid_host_init(classic_descriptors, sizeof(classic_descriptors));
    hid_host_register_packet_handler(input_hid_event);
    hids_host_init(le_descriptors, sizeof(le_descriptors));
    input_hci_registration.callback = input_hci_event;
    hci_add_event_handler(&input_hci_registration);
    input_sm_registration.callback = input_sm_event;
    sm_add_event_handler(&input_sm_registration);
    btstack_run_loop_set_timer_handler(&input_timer, input_tick);
}

void hub_input_start(void) {
    if (input_started) return;
    input_started = true;
    load_targets();
    uint8_t status = gap_load_resolving_list_from_le_device_db();
    resolving_list_supported = status == ERROR_CODE_SUCCESS;
    if (status) printf("[input] controller privacy resolving list status=0x%02x\n", status);
    btstack_run_loop_set_timer(&input_timer, 100);
    btstack_run_loop_add_timer(&input_timer);
    printf("[input] two persistent targets; boot reconnect is bond-only, unknown peers need enroll/connect\n");
}

void hub_input_scan_stop(void) {
    if (scanning == SCAN_LE) {
        gap_stop_scan();
        scanning = SCAN_OFF;
        printf("[scan BLE] stopped\n");
    } else if (scanning == SCAN_CLASSIC) {
        scanning = SCAN_CLASSIC_STOPPING;
        // Cancelling before Inquiry command status is unsafe in this SDK.
        if (classic_inquiry_active) {
            int status = gap_inquiry_stop();
            printf("[scan Classic] stop status=0x%02x\n", status);
        } else {
            printf("[scan Classic] stop pending inquiry command completion\n");
        }
    }
}

bool hub_input_enrolling(void) {
    return enrollment_active || enrollment_pending;
}

void hub_input_enroll_stop(void) {
    bool was_enrolling = enrollment_active || enrollment_pending;
    enrollment_active = false;
    enrollment_pending = false;
    pending_candidate = -1;
    if (was_enrolling) hub_input_scan_stop();
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        input_slot_t *slot = &inputs[i];
        if (!slot->selected || !slot->candidate) continue;
        slot->pairing_authorized = false;
        if (slot->state != INPUT_READY) {
            if (!slot->classic && slot->handle != HCI_CON_HANDLE_INVALID &&
                !slot->security_complete) sm_bonding_decline(slot->handle);
            close_input(slot, 0, false);
        }
    }
    if (was_enrolling) {
        printf("[input] enrollment closed; unfinished candidates cancelled, saved targets kept\n");
    }
}

bool hub_input_enroll_start(void) {
    if (HUB_CLASSIC_OPEN_TEST) {
        printf("[open-test] automatic enrollment disabled in this experimental image\n");
        return false;
    }
    if (!input_started || !input_tlv || hub_security_input_pairing_blocked() ||
        establishing >= 0 || forgetting || any_closing()) {
        printf("[input] enroll rejected: controller/storage unavailable or pairing/disconnect busy\n");
        return false;
    }
    if (enrollment_active) {
        printf("[input] enrollment already active; the absolute 60-second deadline is unchanged\n");
        return false;
    }
    bool empty = false;
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        if (!inputs[i].selected && !corrupt_target[i]) empty = true;
    }
    if (!empty) {
        printf("[input] enroll rejected: no missing keyboard/mouse capacity; input forget N or inputs reset first\n");
        enrollment_pending = false;
        return false;
    }
    enrollment_pending = false;
    enrollment_active = true;
    enrollment_deadline = btstack_run_loop_get_time_ms() + HUB_INPUT_ENROLLMENT_MS;
    enrollment_next_scan = btstack_run_loop_get_time_ms();
    enrollment_next_classic = false;
    candidate_count = 0;
    memset(scan_peers, 0, sizeof(scan_peers));
    next_scan_peer = 0;
    pending_candidate = -1;
    hub_input_scan_stop();
    printf("[input] enrollment open for 60 seconds; put keyboard/mouse in pairing mode (reserved roles=%u)\n",
           reserved_roles(NULL));
    return true;
}

bool hub_input_forget(unsigned slot_mask) {
    if (!slot_mask || (slot_mask & ~((1u << HUB_INPUT_COUNT) - 1u)) ||
        !input_started || !input_tlv || forgetting) {
        printf("[input] forget rejected: invalid mask, storage/controller not ready, or prior forget pending\n");
        return false;
    }
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        if ((slot_mask & (1u << i)) && corrupt_target[i] && !inputs[i].identity_valid) {
            printf("[input %u] forget unsupported: corrupt record has no trustworthy identity; no unrelated keys deleted\n",
                   i + 1);
            return false;
        }
    }
    hub_input_enroll_stop();
    hub_input_scan_stop();
    forgetting = slot_mask;
    enrollment_pending = true;
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        if (!(slot_mask & (1u << i))) continue;
        input_slot_t *slot = &inputs[i];
        if (!slot->classic && slot->handle != HCI_CON_HANDLE_INVALID) {
            (void)capture_identity(slot, sm_le_device_index(slot->handle));
        }
        slot->selected = true;
        slot->forget = true;
        slot->remove = true;
        close_input(slot, 0, false);
    }
    printf("[input] forget mask=%u queued; replacement enrollment waits for disconnects and PC keyboard security idle\n",
           slot_mask);
    return true;
}

static void enrollment_tick(uint32_t now) {
    if (forgetting || establishing >= 0 || any_closing() || hub_security_input_pairing_blocked()) return;
    if (enrollment_pending && scanning == SCAN_OFF) {
        (void)hub_input_enroll_start();
    }
    if (!enrollment_permitted() || scanning != SCAN_OFF) return;
    if (pending_candidate >= 0) {
        input_candidate_t *candidate = &candidates[pending_candidate];
        pending_candidate = -1;
        for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
            input_slot_t *slot = &inputs[i];
            if (slot->selected || corrupt_target[i]) continue;
            reset_slot(slot);
            slot->selected = true;
            slot->candidate = true;
            slot->classic = candidate->classic;
            slot->address_type = candidate->type;
            memcpy(slot->address, candidate->address, 6);
            slot->pairing_authorized = true;
            (void)start_connection(slot);
            return;
        }
        printf("[input] enrollment capacity exhausted\n");
        hub_input_enroll_stop();
        return;
    }
    if (!expired(now, enrollment_next_scan)) return;
    if (candidate_count == INPUT_CANDIDATES) {
        printf("[input] enrollment candidate limit reached; enroll again to try more devices\n");
        hub_input_enroll_stop();
        return;
    }
    bool classic = enrollment_next_classic;
    enrollment_next_classic = !classic;
    enrollment_next_scan = now + INPUT_ENROLL_SCAN_MS + 250u;
    if (classic) {
        scanning = SCAN_CLASSIC;
        classic_inquiry_active = false;
        int status = gap_inquiry_start(3);
        if (status) {
            printf("[input] enrollment inquiry start failed status=0x%02x\n", status);
            scanning = SCAN_OFF;
            return;
        }
    } else {
        gap_set_scan_parameters(1, 96, 48);
        scanning = SCAN_LE;
        gap_start_scan();
    }
    scan_deadline = now + INPUT_ENROLL_SCAN_MS;
    printf("[input] enrollment scanning %s\n", classic ? "Classic" : "BLE");
}

void hub_input_scan(bool classic) {
    if (!input_started || establishing >= 0 || scanning != SCAN_OFF ||
        hub_input_enrolling() || forgetting || any_closing()) {
        printf("[input] scan rejected: controller not ready or discovery/scan busy\n");
        return;
    }
    if (classic) {
        scanning = SCAN_CLASSIC;
        classic_inquiry_active = false;
        int status = gap_inquiry_start(10);
        if (status) {
            printf("[scan Classic] start failed status=0x%02x\n", status);
            scanning = SCAN_OFF;
            return;
        }
    } else {
        // Active scan obtains scan-response names, with a modest 25% duty cycle.
        gap_set_scan_parameters(1, 96, 24);
        gap_start_scan();
        scanning = SCAN_LE;
    }
    scan_deadline = btstack_run_loop_get_time_ms() + INPUT_SCAN_MS;
    printf("[scan %s] discovery only; use connect with the displayed address/type\n",
           classic ? "Classic" : "BLE");
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool connect_input(unsigned index, bool classic, uint8_t address_type,
                          const char *address_text, bool open_test) {
    if (HUB_CLASSIC_OPEN_TEST && classic && !open_test) {
        printf("[open-test] use open-test <slot> <address> for Classic input in this image\n");
        return false;
    }
    if (open_test && (!HUB_CLASSIC_OPEN_TEST || !classic)) {
        printf("[open-test] disabled in normal firmware or invalid transport\n");
        return false;
    }
    if (hub_security_input_pairing_blocked()) {
        printf("[input] finish PC keyboard pairing (pair off) before pairing more inputs\n");
        return false;
    }
    bd_addr_t address;
    if (index >= HUB_INPUT_COUNT || !address_text || strlen(address_text) != 17 ||
        address_type > 1 || (classic && address_type != 0)) {
        printf("[input] connect rejected: use slot 1..2 and a valid address/type in the console\n");
        return false;
    }
    for (unsigned i = 0; i < 6; ++i) {
        int hi = hex_digit(address_text[3 * i]), lo = hex_digit(address_text[3 * i + 1]);
        if (hi < 0 || lo < 0 || (i != 5 && address_text[3 * i + 2] != ':')) {
            printf("[input] connect rejected: invalid address\n");
            return false;
        }
        address[i] = (uint8_t)((hi << 4) | lo);
    }
    const uint8_t zero[6] = {0}, ones[6] = {255, 255, 255, 255, 255, 255};
    if (!memcmp(address, zero, 6) || !memcmp(address, ones, 6)) {
        printf("[input] connect rejected: invalid peer address\n");
        return false;
    }
    input_slot_t *slot = &inputs[index];
    if (open_test && slot->stored) {
        printf("[open-test] choose an empty input slot; saved targets are not replaced\n");
        return false;
    }
    if (open_test) {
        link_key_t key;
        link_key_type_t type;
        if (gap_get_link_key_for_bd_addr(address, key, &type)) {
            printf("[open-test] existing bond would mask a no-authentication test; bond left untouched\n");
            return false;
        }
    }
    if (!input_started || establishing >= 0 || scanning != SCAN_OFF ||
        hub_input_enrolling() || forgetting || corrupt_target[index] ||
        (slot->selected && slot->state != INPUT_FAILED && slot->state != INPUT_RETRY) ||
        slot->handle != HCI_CON_HANDLE_INVALID || slot->cid) {
        printf("[input %u] connect rejected: busy/not ready; stop scan or disconnect existing slot first\n", index + 1);
        return false;
    }
    if (slot->stored && (slot->classic != classic ||
        !(((slot->address_type & 1u) == address_type && !memcmp(slot->address, address, 6)) ||
          (slot->identity_type == address_type && !memcmp(slot->identity, address, 6))))) {
        printf("[input %u] connect rejected: forget the saved target before replacing it\n", index + 1);
        return false;
    }
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        if (inputs[i].selected && inputs[i].state == INPUT_CLOSING) {
            printf("[input %u] connect rejected: slot %u disconnect still pending\n", index + 1, i + 1);
            return false;
        }
        if (i != index && inputs[i].selected && inputs[i].classic == classic &&
            (((inputs[i].address_type & 1u) == address_type && !memcmp(inputs[i].address, address, 6)) ||
             (inputs[i].identity_valid && inputs[i].identity_type == address_type &&
              !memcmp(inputs[i].identity, address, 6)))) {
            printf("[input %u] connect rejected: peer already selected in slot %u\n", index + 1, i + 1);
            return false;
        }
    }
    bool stored = slot->stored;
    if (open_test && open_bridge_enabled) {
        hub_input_set_open_bridge(false);
    }
    hub_input_target_t target = {
        .classic = slot->classic, .type = slot->identity_type, .roles = slot->roles
    };
    memcpy(target.address, slot->identity, 6);
    release_input(slot);
    reset_slot(slot);
    slot->selected = true;
    slot->classic = classic;
    slot->address_type = address_type;
    memcpy(slot->address, address, 6);
    if (stored) {
        slot->stored = true;
        slot->identity_valid = true;
        slot->identity_type = target.type;
        memcpy(slot->identity, target.address, 6);
        slot->roles = target.roles;
        slot->ever_ready = true;
    }
    slot->open_test = open_test;
    slot->pairing_authorized = !open_test;
    if (open_test) {
        if (!bond_capacity_available(slot)) {
            printf("[open-test] key database full; refusing any risk of evicting existing keys\n");
            reset_slot(slot);
            return false;
        }
        printf("[open-test] local Classic Level 0; no PIN will be supplied and reports will not be forwarded\n");
    }
    return start_connection(slot);
}

bool hub_input_connect(unsigned index, bool classic, uint8_t address_type,
                       const char *address_text) {
    return connect_input(index, classic, address_type, address_text, false);
}

bool hub_input_open_test_connect(unsigned index, const char *address_text) {
    return connect_input(index, true, 0, address_text, true);
}

bool hub_input_open_test_peer(const uint8_t address[6]) {
    input_slot_t *slot = slot_for_classic_address(address);
    return slot && slot->open_test && !slot->remove && slot->state != INPUT_CLOSING
        && slot->state != INPUT_FAILED;
}

bool hub_input_open_bridge_enabled(void) {
    return open_bridge_enabled;
}

bool hub_input_set_open_bridge(bool enabled) {
    if (!HUB_CLASSIC_OPEN_TEST) {
        printf("[bridge] this opt-in is only for Level 0 input firmware\n");
        return false;
    }
    if (enabled) {
        bool ready = false;
        for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
            ready |= inputs[i].selected && inputs[i].open_test && inputs[i].state == INPUT_READY;
        }
        if (!ready || hub_security_input_pairing_blocked()) {
            printf("[bridge] first connect the Level 0 input and finish pending PC pairing\n");
            return false;
        }
    }
    if (enabled == open_bridge_enabled) return true;
    open_bridge_enabled = enabled;
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        if (!inputs[i].selected || !inputs[i].open_test) continue;
        if (enabled && inputs[i].state == INPUT_READY) hub_router_input_connected(i);
        else hub_router_input_disconnected(i);
    }
    printf("[bridge] unauthenticated-input forwarding %s; keyboard radio link remains unencrypted\n",
           enabled ? "ON by explicit operator choice" : "OFF");
    return true;
}

void hub_input_disconnect(unsigned index) {
    if (index >= HUB_INPUT_COUNT) {
        printf("[input] disconnect rejected: use slot 1..2 in the console\n");
        return;
    }
    input_slot_t *slot = &inputs[index];
    if (slot->forget) {
        printf("[input %u] forget already pending\n", index + 1);
        return;
    }
    release_input(slot);
    if (!slot->selected) {
        printf("[input %u] already empty\n", index + 1);
        return;
    }
    slot->paused = true;
    slot->remove = !slot->stored && !slot->storage_error;
    if (slot->stored) {
        printf("[input %u] disconnect pauses this boot; saved identity/bond kept (input forget deletes them)\n", index + 1);
    }
    close_input(slot, 0, false);
}

bool hub_input_is_connection(uint16_t handle) {
    return slot_for_handle(handle) != NULL;
}

bool hub_input_is_expected_classic(const uint8_t address[6]) {
    input_slot_t *slot = slot_for_classic_address(address);
    // Main uses this predicate for address-only PIN/SSP events before HID opens.
    return slot && slot->pairing_authorized && !slot->remove && !slot->auth_failed &&
           slot->state != INPUT_CLOSING && !hub_security_input_pairing_blocked() &&
           !expired(btstack_run_loop_get_time_ms(), slot->deadline) &&
           (!slot->candidate || enrollment_permitted());
}

bool hub_input_allow_pairing(uint16_t handle) {
    input_slot_t *slot = slot_for_handle(handle);
    return slot && slot->pairing_authorized && !slot->remove && !slot->auth_failed &&
           slot->state != INPUT_CLOSING && slot->state != INPUT_FAILED &&
           !hub_security_input_pairing_blocked() &&
           !expired(btstack_run_loop_get_time_ms(), slot->deadline) &&
           (!slot->candidate || enrollment_permitted());
}

bool hub_input_keyboard_ready(void) {
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        input_slot_t *slot = &inputs[i];
        if (!slot->selected || slot->remove || (slot->open_test && !open_bridge_enabled)
            || slot->state != INPUT_READY) continue;
        for (unsigned j = 0; j < slot->report_count; ++j) {
            if (slot->reports[j].keyboard && (slot->roles & HUB_INPUT_ROLE_KEYBOARD)) return true;
        }
    }
    return false;
}

bool hub_input_pairing_busy(void) {
    return establishing >= 0 || hub_input_enrolling() || forgetting || any_closing();
}

void hub_input_status(void) {
    static const char *const states[] = {
        "empty", "connecting", "security", "HID-discovery", "HID-ready",
        "disconnect-pending", "reconnect-backoff", "failed"
    };
    printf("[input] scan=%u establishing-slot=%d (0=none) targets=persistent enrollment=%s forgetting=%u\n",
           (unsigned)scanning, establishing + 1,
           enrollment_active ? "active" : enrollment_pending ? "deferred" : "closed", forgetting);
    for (unsigned i = 0; i < HUB_INPUT_COUNT; ++i) {
        input_slot_t *slot = &inputs[i];
        if (!slot->selected) {
            printf("[input %u] %s\n", i + 1, corrupt_target[i] ? "corrupt target; blocked" : "empty");
            continue;
        }
        printf("[input %u] %s %s type=%s(%u) state=%s handle=0x%04x HID-cid=0x%04x "
               "pair=%s retry=%u/%u last=0x%02x saved=%u roles=%u%s\n",
               i + 1, slot->classic ? "Classic" : "BLE", bd_addr_to_str(slot->address),
               (slot->address_type & 1u) ? "random/identity" : "public/identity", slot->address_type,
               states[slot->state], slot->handle, slot->cid,
               slot->open_test ? "not-required" : slot->pairing_authorized ? "authorized" : "bond-only",
               slot->retries, INPUT_RETRIES, slot->last_error, slot->stored, slot->roles,
               slot->remove ? " removing" : slot->paused ? " paused" : "");
        if (slot->identity_valid) {
            printf("[input %u] identity=%s type=%u\n", i + 1,
                   bd_addr_to_str(slot->identity), slot->identity_type);
        }
        printf("[input %u] last-auth=0x%02x last-HID=0x%02x last-disconnect=0x%02x "
               "security-complete=%u descriptor-valid=%u (0xff=not observed)\n",
               i + 1, slot->last_auth_status, slot->last_hid_status,
               slot->last_disconnect_reason, slot->security_complete, slot->hid_validated);
        if (slot->open_test) {
            printf("[open-test] input %u reports=%" PRIu32 " encryption-key=%u forwarding=%s saved-target=0\n",
                   i + 1, slot->test_reports, slot->handle == HCI_CON_HANDLE_INVALID
                       ? 0 : gap_encryption_key_size(slot->handle),
                   open_bridge_enabled ? "ON" : "OFF");
        }
    }
}
