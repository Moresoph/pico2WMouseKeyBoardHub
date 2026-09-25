#include <string.h>
#include "hub_classic_mux.h"
#include "bluetooth_psm.h"
#include "btstack_event.h"
#include "btstack_run_loop.h"
#include "hci.h"
#include "l2cap.h"

/* These predicates authorize addresses, never "everything except a keyboard". */
extern bool hub_input_is_classic_target(const uint8_t address[6]);
extern bool hub_classic_output_accept_connection(const uint8_t address[6]);

#define MUX_CHANNELS MAX_NR_L2CAP_CHANNELS
#define MUX_SECURITY_TIMEOUT_MS 5000u
#define MUX_POLL_MS 100u
#define MUX_INCOMING_EVENT_SIZE 16u

typedef enum { MUX_NONE, MUX_HOST, MUX_DEVICE } mux_role_t;
typedef struct {
    mux_role_t role;
    uint16_t cid, handle, psm;
    uint32_t generation, deadline;
    bd_addr_t address;
    bool forwarded, open, blocked;
    uint8_t incoming[MUX_INCOMING_EVENT_SIZE];
} mux_channel_t;

static mux_channel_t channels[MUX_CHANNELS];
static btstack_packet_handler_t handlers[3][2];
static uint16_t requested_mtu[3][2];
static gap_security_level_t requested_level[3][2];
static uint32_t generation;
static bool registration_failed, started, timer_active;
static btstack_timer_source_t security_timer;
static btstack_packet_callback_registration_t hci_registration;

static void mux_packet_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size);
static void mux_hci_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size);
static void mux_timer_handler(btstack_timer_source_t *timer);

static int psm_index(uint16_t psm) {
    if (psm == BLUETOOTH_PSM_HID_CONTROL) return 0;
    if (psm == BLUETOOTH_PSM_HID_INTERRUPT) return 1;
    return -1;
}

static mux_channel_t *find_channel(uint16_t cid) {
    for (unsigned i = 0; i < MUX_CHANNELS; ++i)
        if (channels[i].role != MUX_NONE && channels[i].cid == cid) return &channels[i];
    return NULL;
}

static void clear_generation(uint16_t cid, uint32_t expected) {
    mux_channel_t *entry = find_channel(cid);
    if (entry && entry->generation == expected) memset(entry, 0, sizeof(*entry));
}

static bool secure(uint16_t handle) {
    return gap_security_level(handle) >= LEVEL_2 && gap_encryption_key_size(handle) == 16;
}

static gap_security_level_t required_level(const mux_channel_t *entry) {
    return entry->role == MUX_DEVICE ? LEVEL_2 : requested_level[MUX_HOST][psm_index(entry->psm)];
}

static bool channel_secure(const mux_channel_t *entry) {
    if (entry->role == MUX_DEVICE) return secure(entry->handle);
    return gap_security_level(entry->handle) >= required_level(entry);
}

static bool authorized(const mux_channel_t *entry) {
    if (entry->role == MUX_HOST) return hub_input_is_classic_target(entry->address);
    return !hub_input_is_classic_target(entry->address) &&
           hub_classic_output_accept_connection(entry->address);
}

static void forward(mux_channel_t *entry, uint8_t type, uint8_t *packet, uint16_t size) {
    /* HID host prepends seven bytes to incoming data in BTstack's original buffer. */
    handlers[entry->role][psm_index(entry->psm)](type, entry->cid, packet, size);
}

void hub_classic_mux_decline(uint16_t cid) {
    mux_channel_t *entry = find_channel(cid);
    uint32_t expected = entry ? entry->generation : 0;
    if (entry) entry->blocked = true;
    l2cap_decline_connection(cid);
    /* BTstack frees declined channels without sending CHANNEL_CLOSED. */
    if (expected) clear_generation(cid, expected);
}

static void stop_peer(mux_role_t role, uint16_t handle) {
    bool found = false, already_blocked = false;
    for (unsigned i = 0; i < MUX_CHANNELS; ++i) {
        mux_channel_t *entry = &channels[i];
        if (entry->role != role || entry->handle != handle) continue;
        found = true;
        already_blocked |= entry->blocked;
        entry->blocked = true;
    }
    for (unsigned i = 0; i < MUX_CHANNELS; ++i) {
        mux_channel_t *entry = &channels[i];
        if (entry->role == role && entry->handle == handle && !entry->forwarded)
            hub_classic_mux_decline(entry->cid);
    }
    if (found && !already_blocked) (void)gap_disconnect(handle);
}

static void stop_device(uint16_t handle) {
    stop_peer(MUX_DEVICE, handle);
}

bool hub_classic_mux_output_ready(uint16_t handle) {
    unsigned open_mask = 0;
    if (!started || handle == HCI_CON_HANDLE_INVALID || !secure(handle)) return false;
    for (unsigned i = 0; i < MUX_CHANNELS; ++i) {
        const mux_channel_t *entry = &channels[i];
        if (entry->role != MUX_DEVICE) continue;
        if (entry->handle != handle || entry->blocked || !authorized(entry)) return false;
        if (entry->open) open_mask |= 1u << psm_index(entry->psm);
    }
    return open_mask == 3;
}

bool hub_classic_mux_output_pending(void) {
    for (unsigned i = 0; i < MUX_CHANNELS; ++i)
        if (channels[i].role == MUX_DEVICE) return true;
    return false;
}

uint8_t hub_classic_mux_send(const uint8_t *packet, uint16_t size) {
    if (!packet || !size) return ERROR_CODE_INVALID_HCI_COMMAND_PARAMETERS;
    for (unsigned i = 0; i < MUX_CHANNELS; ++i) {
        mux_channel_t *entry = &channels[i];
        if (entry->role != MUX_DEVICE || entry->psm != BLUETOOTH_PSM_HID_INTERRUPT) continue;
        if (!hub_classic_mux_output_ready(entry->handle)) {
            if (!secure(entry->handle) || !authorized(entry)) stop_device(entry->handle);
            return ERROR_CODE_COMMAND_DISALLOWED;
        }
        if (size > l2cap_get_remote_mtu_for_local_cid(entry->cid))
            return L2CAP_DATA_LEN_EXCEEDS_REMOTE_MTU;
        return l2cap_send(entry->cid, packet, size);
    }
    return ERROR_CODE_COMMAND_DISALLOWED;
}

static void arm_timer(void) {
    if (timer_active) return;
    timer_active = true;
    btstack_run_loop_set_timer(&security_timer, MUX_POLL_MS);
    btstack_run_loop_add_timer(&security_timer);
}

static void deliver_incoming(mux_channel_t *entry) {
    uint8_t event[MUX_INCOMING_EVENT_SIZE];
    memcpy(event, entry->incoming, sizeof(event));
    entry->forwarded = true;
    forward(entry, HCI_EVENT_PACKET, event, sizeof(event));
}

static void incoming_connection(uint8_t *packet, uint16_t size) {
    if (size < MUX_INCOMING_EVENT_SIZE) return;
    uint16_t cid = l2cap_event_incoming_connection_get_local_cid(packet);
    uint16_t handle = l2cap_event_incoming_connection_get_handle(packet);
    uint16_t psm = l2cap_event_incoming_connection_get_psm(packet);
    bd_addr_t address;
    l2cap_event_incoming_connection_get_address(packet, address);
    bool host = hub_input_is_classic_target(address);
    bool device = hub_classic_output_accept_connection(address);
    if (!started || psm_index(psm) < 0 || !cid || handle == HCI_CON_HANDLE_INVALID ||
        host == device) {
        l2cap_decline_connection(cid);
        return;
    }
    mux_role_t role = host ? MUX_HOST : MUX_DEVICE;
    mux_channel_t *available = NULL;
    for (unsigned i = 0; i < MUX_CHANNELS; ++i) {
        mux_channel_t *entry = &channels[i];
        if (entry->role == MUX_NONE) {
            if (!available) available = entry;
            continue;
        }
        /* A CID already assigned by the stack cannot be a second connection.
           Do not decline it: that would destroy the original live channel. */
        if (entry->cid == cid) return;
        bool same_address = memcmp(entry->address, address, sizeof(address)) == 0;
        if ((entry->handle == handle && (!same_address || entry->role != role ||
                                        entry->psm == psm || entry->blocked)) ||
            (same_address && entry->handle != handle) ||
            (role == MUX_DEVICE && entry->role == MUX_DEVICE &&
             (!same_address || entry->handle != handle))) {
            l2cap_decline_connection(cid);
            return;
        }
    }
    if (!available) {
        l2cap_decline_connection(cid);
        return;
    }
    memset(available, 0, sizeof(*available));
    available->role = role;
    available->cid = cid;
    available->handle = handle;
    available->psm = psm;
    if (!++generation) ++generation;
    available->generation = generation;
    memcpy(available->address, address, sizeof(address));
    memcpy(available->incoming, packet, MUX_INCOMING_EVENT_SIZE);
    if (required_level(available) > LEVEL_0) arm_timer();
    if (channel_secure(available)) {
        deliver_incoming(available);
    } else {
        /* LEVEL_0 bypasses BTstack's Pending response. A peer may time out and
           need to retry; do not retain an unaccepted request past five seconds. */
        available->deadline = btstack_run_loop_get_time_ms() + MUX_SECURITY_TIMEOUT_MS;
        gap_security_level_t level = required_level(available);
        bool requested = false;
        for (unsigned i = 0; i < MUX_CHANNELS; ++i)
            if (&channels[i] != available && channels[i].role == role &&
                channels[i].handle == handle && required_level(&channels[i]) >= level)
                requested = true;
        if (!requested) gap_request_security_level(handle, level);
    }
}

static void mux_packet_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    uint8_t event = 0;
    uint16_t cid = channel;
    if (type == HCI_EVENT_PACKET) {
        if (size < 2) return;
        event = hci_event_packet_get_type(packet);
        switch (event) {
            case L2CAP_EVENT_INCOMING_CONNECTION:
                incoming_connection(packet, size);
                return;
            case L2CAP_EVENT_CHANNEL_OPENED:
                if (size < 21) return;
                cid = l2cap_event_channel_opened_get_local_cid(packet);
                break;
            case L2CAP_EVENT_CHANNEL_CLOSED:
                if (size < 4) return;
                cid = l2cap_event_channel_closed_get_local_cid(packet);
                break;
            case L2CAP_EVENT_CAN_SEND_NOW:
                if (size < 4) return;
                cid = l2cap_event_can_send_now_get_local_cid(packet);
                break;
            default:
                return;
        }
    } else if (type != L2CAP_DATA_PACKET) {
        return;
    }
    mux_channel_t *entry = find_channel(cid);
    if (!entry) return;
    uint32_t expected = entry->generation;
    uint16_t handle = entry->handle;
    mux_role_t role = entry->role;
    if (event == L2CAP_EVENT_CHANNEL_CLOSED) {
        entry->open = false;
        if (entry->forwarded) forward(entry, type, packet, size);
        clear_generation(cid, expected);
        if (role == MUX_DEVICE) stop_device(handle);
        return;
    }
    if (event == L2CAP_EVENT_CHANNEL_OPENED) {
        if (l2cap_event_channel_opened_get_status(packet)) {
            if (entry->forwarded) forward(entry, type, packet, size);
            clear_generation(cid, expected);
            if (role == MUX_DEVICE) stop_device(handle);
            return;
        }
        bd_addr_t address;
        l2cap_event_channel_opened_get_address(packet, address);
        if (!entry->forwarded || entry->handle != l2cap_event_channel_opened_get_handle(packet) ||
            entry->psm != l2cap_event_channel_opened_get_psm(packet) ||
            memcmp(address, entry->address, sizeof(address))) {
            if (role == MUX_DEVICE) stop_device(handle);
            return;
        }
        entry->open = true;
    }
    if (!entry->forwarded || entry->blocked) return;
    if (required_level(entry) > LEVEL_0) {
        if (!channel_secure(entry) || !authorized(entry)) {
            stop_peer(role, handle);
            return;
        }
        if ((type == L2CAP_DATA_PACKET || event == L2CAP_EVENT_CAN_SEND_NOW) && !entry->open)
            return;
        if (role == MUX_DEVICE && event == L2CAP_EVENT_CAN_SEND_NOW &&
            !hub_classic_mux_output_ready(handle)) return;
    }
    forward(entry, type, packet, size);
}

static void security_changed(uint16_t handle, bool failed, bool final,
                             gap_security_level_t reported_level, bool short_key) {
    for (unsigned i = 0; i < MUX_CHANNELS; ++i) {
        mux_channel_t *entry = &channels[i];
        if (entry->role == MUX_NONE || entry->handle != handle || entry->blocked ||
            required_level(entry) == LEVEL_0) continue;
        /* Another service can report a successful lower-level request while
           our upgrade is pending. Only accepted channels treat that as loss. */
        if (failed || (entry->forwarded && reported_level < required_level(entry)) || !authorized(entry) ||
            (short_key && entry->role == MUX_DEVICE) ||
            (!channel_secure(entry) && (entry->forwarded ||
             (final && reported_level >= required_level(entry))))) {
            stop_peer(entry->role, handle);
            return;
        }
        if (!entry->forwarded && channel_secure(entry)) deliver_incoming(entry);
    }
}

static void mux_hci_handler(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    if (type != HCI_EVENT_PACKET || size < 2) return;
    uint16_t handle;
    switch (hci_event_packet_get_type(packet)) {
        case GAP_EVENT_SECURITY_LEVEL:
            if (size < 6) return;
            security_changed(gap_event_security_level_get_handle(packet),
                             gap_event_security_level_get_status(packet) != 0, true,
                             (gap_security_level_t)gap_event_security_level_get_security_level(packet), false);
            break;
        case HCI_EVENT_ENCRYPTION_CHANGE:
            if (size < 6) return;
            security_changed(hci_event_encryption_change_get_connection_handle(packet),
                             hci_event_encryption_change_get_status(packet) != 0 ||
                             !hci_event_encryption_change_get_encryption_enabled(packet), false, LEVEL_4, false);
            break;
        case HCI_EVENT_ENCRYPTION_CHANGE_V2:
            if (size < 7) return;
            security_changed(hci_event_encryption_change_v2_get_connection_handle(packet),
                             hci_event_encryption_change_v2_get_status(packet) != 0 ||
                             !hci_event_encryption_change_v2_get_encryption_enabled(packet), false, LEVEL_4,
                             hci_event_encryption_change_v2_get_encryption_key_size(packet) != 16);
            break;
        case HCI_EVENT_ENCRYPTION_KEY_REFRESH_COMPLETE:
            if (size < 5) return;
            security_changed(hci_event_encryption_key_refresh_complete_get_handle(packet),
                             hci_event_encryption_key_refresh_complete_get_status(packet) != 0, false, LEVEL_4, false);
            break;
        case HCI_EVENT_AUTHENTICATION_COMPLETE:
            if (size < 5 || !hci_event_authentication_complete_get_status(packet)) return;
            security_changed(hci_event_authentication_complete_get_connection_handle(packet),
                             true, true, LEVEL_4, false);
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            if (size < 6 || hci_event_disconnection_complete_get_status(packet)) return;
            handle = hci_event_disconnection_complete_get_connection_handle(packet);
            for (unsigned i = 0; i < MUX_CHANNELS; ++i) {
                mux_channel_t *entry = &channels[i];
                if (entry->role == MUX_NONE || entry->handle != handle) continue;
                /* HCI observer ordering is not guaranteed. Keep accepted maps
                   for L2CAP's following OPENED(failure)/CLOSED cleanup. */
                if (entry->forwarded) entry->blocked = true;
                else memset(entry, 0, sizeof(*entry));
            }
            break;
        default:
            break;
    }
}

static void mux_timer_handler(btstack_timer_source_t *timer) {
    (void)timer;
    timer_active = false;
    uint32_t now = btstack_run_loop_get_time_ms();
    for (unsigned i = 0; i < MUX_CHANNELS; ++i) {
        mux_channel_t *entry = &channels[i];
        if (entry->role == MUX_NONE || entry->blocked || required_level(entry) == LEVEL_0) continue;
        if (!authorized(entry) || (entry->forwarded && !channel_secure(entry)) ||
            (!entry->forwarded && (int32_t)(now - entry->deadline) >= 0)) {
            stop_peer(entry->role, entry->handle);
        } else if (!entry->forwarded && channel_secure(entry)) {
            deliver_incoming(entry);
        }
    }
    for (unsigned i = 0; i < MUX_CHANNELS; ++i)
        if (channels[i].role != MUX_NONE && required_level(&channels[i]) > LEVEL_0) {
            arm_timer();
            break;
        }
}

static uint8_t capture(mux_role_t role, btstack_packet_handler_t callback,
                       uint16_t psm, uint16_t mtu, gap_security_level_t level) {
    int index = psm_index(psm);
    if (started || index < 0 || !callback || !mtu || level > LEVEL_4 || handlers[role][index]) {
        registration_failed = true;
        return ERROR_CODE_COMMAND_DISALLOWED;
    }
    handlers[role][index] = callback;
    requested_mtu[role][index] = mtu;
    requested_level[role][index] = level;
    return ERROR_CODE_SUCCESS;
}

uint8_t hub_classic_mux_register_host(btstack_packet_handler_t callback,
                                    uint16_t psm, uint16_t mtu, gap_security_level_t level) {
    return capture(MUX_HOST, callback, psm, mtu, level);
}

uint8_t hub_classic_mux_register_device(btstack_packet_handler_t callback,
                                      uint16_t psm, uint16_t mtu, gap_security_level_t level) {
    return capture(MUX_DEVICE, callback, psm, mtu, level);
}

void hub_classic_mux_init(void) {
    memset(channels, 0, sizeof(channels));
    memset(handlers, 0, sizeof(handlers));
    memset(requested_mtu, 0, sizeof(requested_mtu));
    memset(requested_level, 0, sizeof(requested_level));
    memset(&security_timer, 0, sizeof(security_timer));
    generation = 0;
    registration_failed = false;
    started = false;
    timer_active = false;
    btstack_run_loop_set_timer_handler(&security_timer, mux_timer_handler);
    hci_registration.callback = mux_hci_handler;
    hci_add_event_handler(&hci_registration);
}

bool hub_classic_mux_start(void) {
    if (started) return !registration_failed;
    if (registration_failed) return false;
    for (unsigned role = MUX_HOST; role <= MUX_DEVICE; ++role)
        for (unsigned index = 0; index < 2; ++index)
            if (!handlers[role][index]) return false;
    for (unsigned index = 0; index < 2; ++index) {
        uint16_t mtu = requested_mtu[MUX_HOST][index];
        if (requested_mtu[MUX_DEVICE][index] > mtu) mtu = requested_mtu[MUX_DEVICE][index];
        /* Match the host's effective 0xffff request without advertising more
           than the stack can receive. Device's 100 must not truncate input. */
        if (mtu > l2cap_max_mtu()) mtu = l2cap_max_mtu();
        uint16_t psm = index ? BLUETOOTH_PSM_HID_INTERRUPT : BLUETOOTH_PSM_HID_CONTROL;
        if (l2cap_register_service(mux_packet_handler, psm, mtu, LEVEL_0)) {
            if (index) (void)l2cap_unregister_service(BLUETOOTH_PSM_HID_CONTROL);
            registration_failed = true;
            return false;
        }
    }
    started = true;
    return true;
}
