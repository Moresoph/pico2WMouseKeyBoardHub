#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Compile the production dispatcher; mock only its profile/controller boundary. */
#include "..\src\hub_classic_mux.c"

static uint32_t now_ms;
static unsigned registrations, unregisters, declines, disconnects, security_requests, sends;
static unsigned fail_registration;
static uint16_t last_decline, last_disconnect, last_send, remote_mtu;
static uint8_t send_status;
static gap_security_level_t host_capture_level, last_requested_level;
static bool host_declines, synchronous_open, allow_pc, allow_second_pc, overlap;
static bool reuse_on_close;
static uint8_t *last_data;
static btstack_packet_handler_t service_handlers[2];
static btstack_packet_handler_t observer;
static struct { unsigned incoming, opened, closed, can_send, data; } calls[3];
static gap_security_level_t levels[64];
static uint8_t keys[64];
static bd_addr_t keyboard = {1, 2, 3, 4, 5, 6};
static bd_addr_t computer = {2, 2, 3, 4, 5, 6};
static bd_addr_t computer2 = {3, 2, 3, 4, 5, 6};
static bd_addr_t unknown = {9, 2, 3, 4, 5, 6};
static uint8_t reused_event[MUX_INCOMING_EVENT_SIZE];

static void emit_incoming(const uint8_t *address, uint16_t handle, uint16_t cid, uint16_t psm);
static void emit_open(uint16_t cid, uint8_t status);

bool hub_input_is_classic_target(const uint8_t address[6]) {
    return address[0] == 1 || (overlap && address[0] == 2);
}
bool hub_classic_output_accept_connection(const uint8_t address[6]) {
    return (allow_pc && !memcmp(address, computer, 6)) ||
           (allow_second_pc && !memcmp(address, computer2, 6));
}
gap_security_level_t gap_security_level(hci_con_handle_t handle) {
    assert(handle < 64);
    return levels[handle];
}
uint8_t gap_encryption_key_size(hci_con_handle_t handle) {
    assert(handle < 64);
    return keys[handle];
}
void gap_request_security_level(hci_con_handle_t handle, gap_security_level_t level) {
    assert(handle < 64 && level >= LEVEL_1 && level <= LEVEL_4);
    ++security_requests;
    last_requested_level = level;
}
uint8_t gap_disconnect(hci_con_handle_t handle) {
    ++disconnects;
    last_disconnect = handle;
    return 0;
}
void hci_add_event_handler(btstack_packet_callback_registration_t *registration) {
    observer = registration->callback;
}
uint32_t btstack_run_loop_get_time_ms(void) { return now_ms; }
void btstack_run_loop_set_timer(btstack_timer_source_t *timer, uint32_t ms) {
    assert(timer == &security_timer && ms == MUX_POLL_MS);
}
void btstack_run_loop_add_timer(btstack_timer_source_t *timer) {
    assert(timer == &security_timer);
}
void btstack_run_loop_set_timer_handler(btstack_timer_source_t *timer,
                                      void (*handler)(btstack_timer_source_t *)) {
    timer->process = handler;
}
uint16_t l2cap_max_mtu(void) { return 1691; }
uint8_t l2cap_register_service(btstack_packet_handler_t callback, uint16_t psm,
                               uint16_t mtu, gap_security_level_t level) {
    ++registrations;
    assert(level == LEVEL_0 && mtu == l2cap_max_mtu());
    if (registrations == fail_registration) return BTSTACK_MEMORY_ALLOC_FAILED;
    service_handlers[psm_index(psm)] = callback;
    return 0;
}
uint8_t l2cap_unregister_service(uint16_t psm) {
    assert(psm == BLUETOOTH_PSM_HID_CONTROL);
    ++unregisters;
    service_handlers[0] = NULL;
    return 0;
}
void l2cap_decline_connection(uint16_t cid) { ++declines; last_decline = cid; }
uint16_t l2cap_get_remote_mtu_for_local_cid(uint16_t cid) {
    assert(find_channel(cid) && find_channel(cid)->open);
    return remote_mtu;
}
uint8_t l2cap_send(uint16_t cid, const uint8_t *packet, uint16_t size) {
    assert(find_channel(cid) && packet && size);
    assert(hub_classic_mux_output_ready(find_channel(cid)->handle));
    ++sends;
    last_send = cid;
    return send_status;
}

static unsigned mapped(void) {
    unsigned count = 0;
    for (unsigned i = 0; i < MUX_CHANNELS; ++i) count += channels[i].role != MUX_NONE;
    return count;
}

static void profile(mux_role_t role, uint8_t type, uint16_t cid, uint8_t *packet, uint16_t size) {
    mux_channel_t *entry = find_channel(cid);
    assert(entry && entry->role == role && entry->forwarded);
    if (type == L2CAP_DATA_PACKET) {
        assert(size);
        ++calls[role].data;
        last_data = packet;
        if (role == MUX_HOST) memset(packet - 7, 0xa5, 7);
        return;
    }
    assert(type == HCI_EVENT_PACKET);
    switch (packet[0]) {
        case L2CAP_EVENT_INCOMING_CONNECTION:
            ++calls[role].incoming;
            assert(channel_secure(entry));
            if (role == MUX_HOST && host_declines) hub_classic_mux_decline(cid);
            else if (synchronous_open) emit_open(cid, 0);
            break;
        case L2CAP_EVENT_CHANNEL_OPENED: ++calls[role].opened; break;
        case L2CAP_EVENT_CHANNEL_CLOSED:
            ++calls[role].closed;
            if (reuse_on_close) {
                reuse_on_close = false;
                hub_classic_mux_decline(cid);
                mux_packet_handler(HCI_EVENT_PACKET, 0, reused_event, sizeof(reused_event));
            }
            break;
        case L2CAP_EVENT_CAN_SEND_NOW: ++calls[role].can_send; break;
        default: assert(0);
    }
}
static void host_handler(uint8_t type, uint16_t cid, uint8_t *packet, uint16_t size) {
    profile(MUX_HOST, type, cid, packet, size);
}
static void device_handler(uint8_t type, uint16_t cid, uint8_t *packet, uint16_t size) {
    profile(MUX_DEVICE, type, cid, packet, size);
}

static void capture_profiles(void) {
    assert(!hub_classic_mux_register_device(device_handler, BLUETOOTH_PSM_HID_CONTROL, 100, LEVEL_2));
    assert(!hub_classic_mux_register_device(device_handler, BLUETOOTH_PSM_HID_INTERRUPT, 100, LEVEL_2));
    assert(!hub_classic_mux_register_host(host_handler, BLUETOOTH_PSM_HID_INTERRUPT, 0xffff, host_capture_level));
    assert(!hub_classic_mux_register_host(host_handler, BLUETOOTH_PSM_HID_CONTROL, 0xffff, host_capture_level));
}
static void reset(bool start) {
    now_ms = registrations = unregisters = declines = disconnects = security_requests = sends = 0;
    fail_registration = 0;
    last_decline = last_disconnect = last_send = 0;
    remote_mtu = 100;
    send_status = 0;
    host_capture_level = last_requested_level = LEVEL_0;
    host_declines = synchronous_open = allow_second_pc = overlap = reuse_on_close = false;
    allow_pc = true;
    last_data = NULL;
    memset(calls, 0, sizeof(calls));
    memset(levels, 0, sizeof(levels));
    memset(keys, 0, sizeof(keys));
    memset(service_handlers, 0, sizeof(service_handlers));
    hub_classic_mux_init();
    if (start) {
        capture_profiles();
        assert(hub_classic_mux_start());
    }
}
static void make_incoming(uint8_t *event, const uint8_t *address, uint16_t handle,
                          uint16_t cid, uint16_t psm) {
    memset(event, 0, MUX_INCOMING_EVENT_SIZE);
    event[0] = L2CAP_EVENT_INCOMING_CONNECTION;
    event[1] = MUX_INCOMING_EVENT_SIZE - 2;
    reverse_bytes(address, event + 2, 6);
    little_endian_store_16(event, 8, handle);
    little_endian_store_16(event, 10, psm);
    little_endian_store_16(event, 12, cid);
}
static void emit_incoming(const uint8_t *address, uint16_t handle, uint16_t cid, uint16_t psm) {
    uint8_t event[MUX_INCOMING_EVENT_SIZE];
    make_incoming(event, address, handle, cid, psm);
    assert(service_handlers[psm_index(psm)] == mux_packet_handler);
    service_handlers[psm_index(psm)](HCI_EVENT_PACKET, 0, event, sizeof(event));
    memset(event, 0xcc, sizeof(event));
}
static void emit_open(uint16_t cid, uint8_t status) {
    mux_channel_t *entry = find_channel(cid);
    assert(entry);
    uint8_t event[26] = {L2CAP_EVENT_CHANNEL_OPENED, 24};
    event[2] = status;
    reverse_bytes(entry->address, event + 3, 6);
    little_endian_store_16(event, 9, entry->handle);
    little_endian_store_16(event, 11, entry->psm);
    little_endian_store_16(event, 13, cid);
    little_endian_store_16(event, 19, remote_mtu);
    mux_packet_handler(HCI_EVENT_PACKET, 0, event, sizeof(event));
}
static void emit_simple(uint8_t event_type, uint16_t cid) {
    uint8_t event[4] = {0};
    event[0] = event_type;
    event[1] = 2;
    little_endian_store_16(event, 2, cid);
    mux_packet_handler(HCI_EVENT_PACKET, 0, event, sizeof(event));
}
static void emit_security(uint16_t handle, uint8_t status, uint8_t level, uint8_t key) {
    levels[handle] = (gap_security_level_t)level;
    keys[handle] = key;
    uint8_t event[6] = {GAP_EVENT_SECURITY_LEVEL, 4, 0, 0, level, status};
    little_endian_store_16(event, 2, handle);
    observer(HCI_EVENT_PACKET, 0, event, sizeof(event));
}
static void emit_disconnect(uint16_t handle) {
    uint8_t event[6] = {HCI_EVENT_DISCONNECTION_COMPLETE, 4, 0};
    little_endian_store_16(event, 3, handle);
    observer(HCI_EVENT_PACKET, 0, event, sizeof(event));
}
static void pc_pair(bool reverse_open) {
    levels[2] = LEVEL_2;
    keys[2] = 16;
    emit_incoming(computer, 2, 80, BLUETOOTH_PSM_HID_CONTROL);
    emit_incoming(computer, 2, 81, BLUETOOTH_PSM_HID_INTERRUPT);
    assert(!hub_classic_mux_output_ready(2));
    emit_open(reverse_open ? 81 : 80, 0);
    assert(!hub_classic_mux_output_ready(2));
    emit_open(reverse_open ? 80 : 81, 0);
    assert(hub_classic_mux_output_ready(2));
}

static void test_start(void) {
    reset(false);
    assert(!hub_classic_mux_start() && !registrations);
    capture_profiles();
    assert(hub_classic_mux_start() && registrations == 2);
    assert(hub_classic_mux_start() && registrations == 2);
    reset(false);
    capture_profiles();
    fail_registration = 1;
    assert(!hub_classic_mux_start() && !unregisters);
    reset(false);
    capture_profiles();
    fail_registration = 2;
    assert(!hub_classic_mux_start() && unregisters == 1 && !started);
    reset(false);
    capture_profiles();
    assert(hub_classic_mux_register_host(host_handler, BLUETOOTH_PSM_HID_CONTROL, 100, LEVEL_0));
    assert(!hub_classic_mux_start() && !registrations);
}
static void test_interleaved(void) {
    reset(true);
    levels[2] = LEVEL_2; keys[2] = 16;
    emit_incoming(keyboard, 1, 70, BLUETOOTH_PSM_HID_CONTROL);
    emit_incoming(computer, 2, 80, BLUETOOTH_PSM_HID_CONTROL);
    emit_open(70, 0);
    emit_incoming(keyboard, 1, 71, BLUETOOTH_PSM_HID_INTERRUPT);
    emit_incoming(computer, 2, 81, BLUETOOTH_PSM_HID_INTERRUPT);
    emit_open(81, 0);
    emit_simple(L2CAP_EVENT_CAN_SEND_NOW, 81);
    assert(!calls[MUX_DEVICE].can_send);
    emit_open(71, 0);
    emit_open(80, 0);
    assert(hub_classic_mux_output_ready(2) && !hub_classic_mux_output_ready(1));
    emit_simple(L2CAP_EVENT_CAN_SEND_NOW, 70);
    emit_simple(L2CAP_EVENT_CAN_SEND_NOW, 81);
    assert(calls[MUX_HOST].can_send == 1 && calls[MUX_DEVICE].can_send == 1);
    uint8_t data[20] = {0};
    mux_packet_handler(L2CAP_DATA_PACKET, 71, data + 7, 8);
    assert(last_data == data + 7 && data[0] == 0xa5 && data[6] == 0xa5);
    mux_packet_handler(L2CAP_DATA_PACKET, 81, data + 7, 8);
    assert(last_data == data + 7 && calls[MUX_DEVICE].data == 1);
    send_status = BTSTACK_ACL_BUFFERS_FULL;
    assert(hub_classic_mux_send(data + 7, 8) == send_status && sends == 1 && last_send == 81);
    remote_mtu = 7;
    assert(hub_classic_mux_send(data + 7, 8) == L2CAP_DATA_LEN_EXCEEDS_REMOTE_MTU && sends == 1);
    assert(hub_classic_mux_send(NULL, 0) == ERROR_CODE_INVALID_HCI_COMMAND_PARAMETERS);
    assert(mapped() == 4 && !disconnects);
}
static void test_rejects(void) {
    reset(true);
    emit_incoming(unknown, 9, 60, BLUETOOTH_PSM_HID_CONTROL);
    assert(declines == 1 && !mapped());
    overlap = true;
    emit_incoming(computer, 2, 61, BLUETOOTH_PSM_HID_CONTROL);
    assert(declines == 2 && !mapped());
    overlap = false;
    pc_pair(false);
    emit_incoming(computer, 2, 82, BLUETOOTH_PSM_HID_CONTROL);
    assert(declines == 3 && !find_channel(82));
    emit_incoming(computer, 3, 83, BLUETOOTH_PSM_HID_CONTROL);
    assert(declines == 4);
    emit_incoming(keyboard, 2, 84, BLUETOOTH_PSM_HID_INTERRUPT);
    assert(declines == 5);
    allow_second_pc = true;
    emit_incoming(computer2, 3, 85, BLUETOOTH_PSM_HID_CONTROL);
    assert(declines == 6 && calls[MUX_DEVICE].incoming == 2);
    emit_incoming(computer, 2, 80, BLUETOOTH_PSM_HID_CONTROL);
    assert(declines == 6 && find_channel(80)->open);
}
static void test_declines_and_reentrancy(void) {
    reset(true);
    host_declines = true;
    emit_incoming(keyboard, 1, 70, BLUETOOTH_PSM_HID_CONTROL);
    assert(declines == 1 && !mapped());
    host_declines = false;
    emit_incoming(keyboard, 1, 70, BLUETOOTH_PSM_HID_CONTROL);
    assert(mapped() == 1);
    hub_classic_mux_decline(70);
    assert(declines == 2 && !mapped());
    synchronous_open = true;
    emit_incoming(keyboard, 1, 70, BLUETOOTH_PSM_HID_CONTROL);
    assert(find_channel(70)->open && calls[MUX_HOST].opened == 1);
    uint32_t previous = find_channel(70)->generation;
    make_incoming(reused_event, keyboard, 3, 70, BLUETOOTH_PSM_HID_CONTROL);
    reuse_on_close = true;
    emit_simple(L2CAP_EVENT_CHANNEL_CLOSED, 70);
    assert(find_channel(70) && find_channel(70)->generation != previous);
    assert(find_channel(70)->handle == 3 && find_channel(70)->open);
    emit_simple(L2CAP_EVENT_CHANNEL_CLOSED, 70);
    assert(!mapped());
}
static void test_security_wait(void) {
    reset(true);
    emit_incoming(computer, 2, 80, BLUETOOTH_PSM_HID_CONTROL);
    emit_incoming(computer, 2, 81, BLUETOOTH_PSM_HID_INTERRUPT);
    assert(security_requests == 1 && !calls[MUX_DEVICE].incoming && mapped() == 2);
    uint8_t data[8] = {0};
    mux_packet_handler(L2CAP_DATA_PACKET, 81, data, sizeof(data));
    emit_simple(L2CAP_EVENT_CAN_SEND_NOW, 81);
    assert(!calls[MUX_DEVICE].data && !calls[MUX_DEVICE].can_send);
    emit_security(2, 0, LEVEL_2, 16);
    assert(calls[MUX_DEVICE].incoming == 2 && !declines);
    emit_open(80, 0); emit_open(81, 0);
    assert(hub_classic_mux_output_ready(2));
    for (unsigned key = 0; key < 16; ++key) {
        reset(true);
        emit_incoming(computer, 2, 80, BLUETOOTH_PSM_HID_CONTROL);
        emit_security(2, 0, LEVEL_2, (uint8_t)key);
        assert(!mapped() && !calls[MUX_DEVICE].incoming && declines == 1 && disconnects == 1);
    }
    reset(true);
    emit_incoming(computer, 2, 80, BLUETOOTH_PSM_HID_CONTROL);
    emit_security(2, 0, LEVEL_1, 16);
    assert(mapped() == 1 && !declines && !calls[MUX_DEVICE].incoming);
    emit_security(2, 0, LEVEL_0, 0);
    assert(mapped() == 1 && !declines && !calls[MUX_DEVICE].incoming);
    emit_security(2, 0, LEVEL_2, 16);
    assert(calls[MUX_DEVICE].incoming == 1 && !declines);
    reset(true);
    emit_incoming(computer, 2, 80, BLUETOOTH_PSM_HID_CONTROL);
    emit_security(2, ERROR_CODE_AUTHENTICATION_FAILURE, LEVEL_2, 16);
    assert(!mapped() && declines == 1 && !calls[MUX_DEVICE].incoming);
    reset(true);
    now_ms = UINT32_MAX - 100;
    emit_incoming(computer, 2, 80, BLUETOOTH_PSM_HID_CONTROL);
    now_ms += MUX_SECURITY_TIMEOUT_MS - 1;
    security_timer.process(&security_timer);
    assert(mapped());
    ++now_ms;
    security_timer.process(&security_timer);
    assert(!mapped() && declines == 1 && !hub_classic_mux_output_pending());
    reset(true);
    emit_incoming(computer, 2, 80, BLUETOOTH_PSM_HID_CONTROL);
    emit_disconnect(2);
    assert(!mapped() && !hub_classic_mux_output_pending() && !calls[MUX_DEVICE].closed);
}
static void test_security_loss(void) {
    reset(true);
    pc_pair(true);
    emit_incoming(keyboard, 1, 70, BLUETOOTH_PSM_HID_CONTROL);
    emit_security(2, 0, LEVEL_0, 0);
    assert(!hub_classic_mux_output_ready(2) && disconnects == 1 && last_disconnect == 2);
    uint8_t data[16] = {0};
    mux_packet_handler(L2CAP_DATA_PACKET, 81, data + 7, 8);
    emit_simple(L2CAP_EVENT_CAN_SEND_NOW, 81);
    assert(!calls[MUX_DEVICE].data && !calls[MUX_DEVICE].can_send);
    assert(hub_classic_mux_send(data, 1) != 0 && !sends);
    assert(disconnects == 1 && find_channel(70));
    emit_simple(L2CAP_EVENT_CHANNEL_CLOSED, 80);
    emit_simple(L2CAP_EVENT_CHANNEL_CLOSED, 81);
    assert(!hub_classic_mux_output_pending() && calls[MUX_DEVICE].closed == 2);
    reset(true);
    pc_pair(false);
    keys[2] = 7;
    assert(!hub_classic_mux_output_ready(2));
    security_timer.process(&security_timer);
    assert(disconnects == 1);
    reset(true);
    pc_pair(false);
    allow_pc = false;
    assert(!hub_classic_mux_output_ready(2));
    assert(hub_classic_mux_send(data, 1) != 0 && disconnects == 1);
}
static void test_hci_security_events(void) {
    reset(true);
    emit_incoming(computer, 2, 80, BLUETOOTH_PSM_HID_CONTROL);
    uint8_t encryption[7] = {HCI_EVENT_ENCRYPTION_CHANGE, 4, 0, 2, 0, 1, 16};
    observer(HCI_EVENT_PACKET, 0, encryption, 6);
    /* Encryption enabled can precede the controller's key-size result. */
    assert(mapped() && !calls[MUX_DEVICE].incoming && !disconnects);
    levels[2] = LEVEL_2; keys[2] = 16;
    observer(HCI_EVENT_PACKET, 0, encryption, 6);
    assert(calls[MUX_DEVICE].incoming == 1);
    emit_open(80, 0);
    encryption[5] = 0;
    observer(HCI_EVENT_PACKET, 0, encryption, 6);
    assert(find_channel(80)->blocked && disconnects == 1);
    reset(true);
    pc_pair(false);
    encryption[0] = HCI_EVENT_ENCRYPTION_CHANGE_V2;
    encryption[1] = 5; encryption[5] = 1; encryption[6] = 15;
    observer(HCI_EVENT_PACKET, 0, encryption, 7);
    assert(!hub_classic_mux_output_ready(2) && disconnects == 1);
    reset(true);
    pc_pair(false);
    uint8_t refresh[5] = {HCI_EVENT_ENCRYPTION_KEY_REFRESH_COMPLETE, 3,
                         ERROR_CODE_AUTHENTICATION_FAILURE, 2, 0};
    observer(HCI_EVENT_PACKET, 0, refresh, sizeof(refresh));
    assert(!hub_classic_mux_output_ready(2) && disconnects == 1);
    reset(true);
    emit_incoming(computer, 2, 80, BLUETOOTH_PSM_HID_CONTROL);
    refresh[0] = HCI_EVENT_AUTHENTICATION_COMPLETE;
    observer(HCI_EVENT_PACKET, 0, refresh, sizeof(refresh));
    assert(!mapped() && declines == 1 && disconnects == 1);
    reset(true);
    emit_incoming(computer, 2, 80, BLUETOOTH_PSM_HID_CONTROL);
    allow_pc = false;
    emit_security(2, 0, LEVEL_2, 16);
    assert(!mapped() && !calls[MUX_DEVICE].incoming && declines == 1);
}
static void strict_host(gap_security_level_t level) {
    reset(false);
    host_capture_level = level;
    capture_profiles();
    assert(hub_classic_mux_start());
}
static void test_captured_host_security(void) {
    strict_host(LEVEL_2);
    emit_incoming(keyboard, 1, 70, BLUETOOTH_PSM_HID_CONTROL);
    assert(!calls[MUX_HOST].incoming && security_requests == 1);
    assert(last_requested_level == LEVEL_2 && !hub_classic_mux_output_pending());
    pc_pair(false);
    assert(!calls[MUX_HOST].incoming && hub_classic_mux_output_ready(2));
    emit_security(1, 0, LEVEL_2, 16);
    assert(calls[MUX_HOST].incoming == 1 && !declines);
    emit_open(70, 0);
    emit_simple(L2CAP_EVENT_CAN_SEND_NOW, 70);
    assert(calls[MUX_HOST].can_send == 1);
    emit_security(1, 0, LEVEL_0, 0);
    assert(disconnects == 1 && last_disconnect == 1 && find_channel(70)->blocked);
    uint8_t data[16] = {0};
    mux_packet_handler(L2CAP_DATA_PACKET, 70, data + 7, 8);
    assert(!calls[MUX_HOST].data && hub_classic_mux_output_ready(2));
    emit_simple(L2CAP_EVENT_CHANNEL_CLOSED, 70);
    assert(!find_channel(70) && calls[MUX_HOST].closed == 1);
    strict_host(LEVEL_3);
    emit_incoming(keyboard, 1, 70, BLUETOOTH_PSM_HID_CONTROL);
    assert(last_requested_level == LEVEL_3 && !calls[MUX_HOST].incoming);
    emit_security(1, 0, LEVEL_2, 16);
    assert(mapped() == 1 && !declines && !calls[MUX_HOST].incoming);
    emit_security(1, ERROR_CODE_AUTHENTICATION_FAILURE, LEVEL_2, 16);
    assert(!mapped() && declines == 1 && last_disconnect == 1);
    strict_host(LEVEL_3);
    emit_incoming(keyboard, 1, 70, BLUETOOTH_PSM_HID_CONTROL);
    emit_security(1, 0, LEVEL_3, 16);
    assert(calls[MUX_HOST].incoming == 1 && !declines);
    strict_host(LEVEL_2);
    emit_incoming(keyboard, 1, 70, BLUETOOTH_PSM_HID_CONTROL);
    now_ms = MUX_SECURITY_TIMEOUT_MS - 1;
    security_timer.process(&security_timer);
    assert(mapped() == 1 && timer_active);
    ++now_ms;
    security_timer.process(&security_timer);
    assert(!mapped() && declines == 1 && !calls[MUX_HOST].incoming && !timer_active);
    strict_host(LEVEL_2);
    emit_incoming(keyboard, 1, 70, BLUETOOTH_PSM_HID_CONTROL);
    emit_disconnect(1);
    assert(!mapped() && !calls[MUX_HOST].incoming);
    reset(true);
    emit_incoming(keyboard, 1, 70, BLUETOOTH_PSM_HID_CONTROL);
    emit_security(1, ERROR_CODE_AUTHENTICATION_FAILURE, LEVEL_0, 0);
    assert(calls[MUX_HOST].incoming == 1 && !security_requests && !disconnects);
}
static void test_close_fail_capacity(void) {
    for (unsigned reverse = 0; reverse < 2; ++reverse) {
        reset(true);
        pc_pair(reverse != 0);
        emit_simple(L2CAP_EVENT_CHANNEL_CLOSED, reverse ? 81 : 80);
        assert(!hub_classic_mux_output_ready(2) && hub_classic_mux_output_pending());
        assert(disconnects == 1);
        emit_simple(L2CAP_EVENT_CHANNEL_CLOSED, reverse ? 80 : 81);
        assert(!mapped());
        pc_pair(reverse == 0);
        assert(hub_classic_mux_output_ready(2));
    }
    reset(true);
    levels[2] = LEVEL_2; keys[2] = 16;
    emit_incoming(computer, 2, 80, BLUETOOTH_PSM_HID_CONTROL);
    emit_open(80, ERROR_CODE_CONNECTION_TIMEOUT);
    assert(!mapped() && calls[MUX_DEVICE].opened == 1 && !hub_classic_mux_output_ready(2));
    emit_incoming(computer, 2, 80, BLUETOOTH_PSM_HID_CONTROL);
    emit_incoming(computer, 2, 81, BLUETOOTH_PSM_HID_INTERRUPT);
    emit_open(80, 0);
    emit_open(81, ERROR_CODE_CONNECTION_TIMEOUT);
    assert(!find_channel(81) && find_channel(80)->blocked && disconnects == 1);
    emit_simple(L2CAP_EVENT_CHANNEL_CLOSED, 80);
    assert(!mapped());
    reset(true);
    pc_pair(false);
    emit_disconnect(2);
    assert(find_channel(80)->blocked && !hub_classic_mux_output_ready(2));
    emit_simple(L2CAP_EVENT_CHANNEL_CLOSED, 81);
    emit_simple(L2CAP_EVENT_CHANNEL_CLOSED, 80);
    assert(!mapped());
    reset(true);
    for (unsigned i = 0; i < MUX_CHANNELS; ++i) {
        bd_addr_t address = {1, 0, 0, 0, 0, (uint8_t)i};
        emit_incoming(address, (uint16_t)i, (uint16_t)(100 + i), BLUETOOTH_PSM_HID_CONTROL);
    }
    assert(mapped() == MUX_CHANNELS);
    emit_incoming(keyboard, 30, 200, BLUETOOTH_PSM_HID_CONTROL);
    assert(declines == 1 && !find_channel(200));
    hub_classic_mux_decline(100);
    emit_incoming(keyboard, 30, 200, BLUETOOTH_PSM_HID_CONTROL);
    assert(mapped() == MUX_CHANNELS && find_channel(200));
}
int main(void) {
    test_start();
    test_interleaved();
    test_rejects();
    test_declines_and_reentrancy();
    test_security_wait();
    test_security_loss();
    test_hci_security_events();
    test_captured_host_security();
    test_close_fail_capacity();
    puts("Classic mux tests passed (routing, security, lifetime, MTU, registration).");
    return 0;
}
