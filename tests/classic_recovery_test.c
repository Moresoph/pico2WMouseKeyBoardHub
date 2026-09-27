#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* Real patched HID/SDP, plus the generated L2CAP reaper; mock transport/allocation only. */
#include "hid_host.c"
#undef BTSTACK_FILE__
#include "sdp_client.c"

static btstack_linked_list_t l2cap_channels;
static unsigned channel_frees;
static void l2cap_free_channel_entry(l2cap_channel_t *channel) {
    ++channel_frees;
    free(channel);
}
#include "l2cap_reap.inc"

static bd_addr_t keyboard = {1, 2, 3, 4, 5, 6};
static bd_addr_t peer = {1, 2, 3, 4, 5, 7};
static hci_connection_t acl;
static bool acl_present;
static uint8_t create_status, last_status;
static unsigned hid_allocations, opened, closed, other_done;
static uint16_t next_cid = 0x40;
static uint8_t descriptors[1024], outgoing[1024];

void btstack_assert_failed(const char *file, uint16_t line) {
    fprintf(stderr, "%s:%u\n", file, line); abort();
}
void hci_dump_log(int level, const char *format, ...) { (void)level; (void)format; }
hci_connection_t *hci_connection_for_bd_addr_and_type(const bd_addr_t address, bd_addr_type_t type) {
    return acl_present && type == BD_ADDR_TYPE_ACL && !bd_addr_cmp(address, keyboard) ? &acl : NULL;
}
hid_host_connection_t *btstack_memory_hid_host_connection_get(void) {
    ++hid_allocations;
    return calloc(1, sizeof(hid_host_connection_t));
}
void btstack_memory_hid_host_connection_free(hid_host_connection_t *connection) {
    assert(hid_allocations); --hid_allocations; free(connection);
}
uint8_t l2cap_create_channel(btstack_packet_handler_t callback, bd_addr_t address,
        uint16_t psm, uint16_t mtu, uint16_t *cid) {
    (void)mtu;
    if (create_status) return create_status;
    l2cap_channel_t *channel = calloc(1, sizeof(*channel));
    channel->channel_type = L2CAP_CHANNEL_TYPE_CLASSIC;
    channel->packet_handler = callback;
    memcpy(channel->address, address, 6);
    channel->local_cid = ++next_cid;
    channel->psm = psm;
    btstack_linked_list_add_tail(&l2cap_channels, (btstack_linked_item_t *)channel);
    if (cid) *cid = channel->local_cid;
    return 0;
}
uint8_t l2cap_disconnect(uint16_t cid) { (void)cid; return 0; }
uint16_t l2cap_max_mtu(void) { return sizeof(outgoing); }
void l2cap_reserve_packet_buffer(void) {}
uint8_t *l2cap_get_outgoing_buffer(void) { return outgoing; }
uint8_t l2cap_send_prepared(uint16_t cid, uint16_t size) { (void)cid; (void)size; return 0; }
uint8_t l2cap_send(uint16_t cid, const uint8_t *data, uint16_t size) {
    (void)cid; (void)data; (void)size; return 0;
}
uint8_t l2cap_request_can_send_now_event(uint16_t cid) { (void)cid; return 0; }
void l2cap_accept_connection(uint16_t cid) { (void)cid; }
void l2cap_decline_connection(uint16_t cid) { (void)cid; }
uint8_t l2cap_register_service(btstack_packet_handler_t cb, uint16_t psm, uint16_t mtu,
        gap_security_level_t security) { (void)cb; (void)psm; (void)mtu; (void)security; return 0; }
gap_security_level_t gap_get_security_level(void) { return LEVEL_0; }

static void application(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel; (void)size;
    assert(type == HCI_EVENT_PACKET && packet[0] == HCI_EVENT_HID_META);
    if (packet[2] == HID_SUBEVENT_CONNECTION_OPENED) {
        ++opened; last_status = hid_subevent_connection_opened_get_status(packet);
    } else if (packet[2] == HID_SUBEVENT_CONNECTION_CLOSED) ++closed;
}
static void other_query(uint8_t type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)type; (void)channel; (void)size;
    if (packet[0] == SDP_EVENT_QUERY_COMPLETE) ++other_done;
}
static void setup(void) {
    assert(!hid_allocations && !l2cap_channels);
    hid_host_init(descriptors, sizeof(descriptors));
    hid_host_register_packet_handler(application);
    sdp_client_init();
    acl_present = false;
    create_status = 0;
    opened = closed = other_done = channel_frees = 0;
}
static void channel_event(btstack_packet_handler_t cb, uint8_t type, uint16_t cid, uint8_t status) {
    uint8_t event[26] = {0};
    event[0] = type;
    if (type == L2CAP_EVENT_CHANNEL_OPENED) {
        event[1] = 24;
        event[2] = status;
        reverse_bd_addr(keyboard, &event[3]);
        little_endian_store_16(event, 13, cid);
        little_endian_store_16(event, 17, 100);
    } else {
        event[1] = 2;
        little_endian_store_16(event, 2, cid);
    }
    cb(HCI_EVENT_PACKET, cid, event, event[1] + 2);
}
static void test_synchronous_failure(void) {
    setup();
    create_status = BTSTACK_MEMORY_ALLOC_FAILED;
    uint16_t cid;
    assert(!hid_host_connect(keyboard, HID_PROTOCOL_MODE_REPORT, &cid));
    assert(opened == 1 && last_status == create_status && !hid_allocations && sdp_client_ready());
    assert(!hid_host_sdp_context_control_cid && !sdp_parser_callback);
    assert(sdp_client_service_search(other_query, keyboard, sdp_service_search_pattern_for_uuid16(0x1124)) == create_status);
    assert(sdp_client_ready() && !sdp_parser_callback);
    assert(sdp_client_service_attribute_search(other_query, keyboard, 1, sdp_client_des_attribute_id_list) == create_status);
    assert(sdp_client_ready() && !sdp_parser_callback);
}
static void test_orphan_and_stale_events(void) {
    setup();
    uint16_t cid;
    assert(!hid_host_connect(keyboard, HID_PROTOCOL_MODE_REPORT, &cid));
    uint16_t old_sdp = sdp_client_sdp_cid;
    assert(old_sdp > 0x40 && hid_allocations == 1 && !sdp_client_ready());
    acl_present = true;
    assert(hub_classic_hid_recover(cid, keyboard) == ERROR_CODE_COMMAND_DISALLOWED);
    assert(!channel_frees && hid_allocations == 1 && !closed);
    acl_present = false;
    assert(!hub_classic_hid_recover(cid, keyboard));
    assert(closed == 1 && !hid_allocations && !l2cap_channels && sdp_client_ready());
    assert(channel_frees == 1 && !sdp_parser_callback);
    uint16_t old_hid = cid;
    assert(!hid_host_connect(keyboard, HID_PROTOCOL_MODE_REPORT, &cid));
    uint16_t fresh_sdp = sdp_client_sdp_cid;
    assert(hub_classic_hid_recover(old_hid, keyboard) == ERROR_CODE_COMMAND_DISALLOWED);
    assert(sdp_client_sdp_cid == fresh_sdp && hid_allocations == 1 && channel_frees == 1);
    channel_event(sdp_client_packet_handler, L2CAP_EVENT_CHANNEL_OPENED, old_sdp, 1);
    channel_event(sdp_client_packet_handler, L2CAP_EVENT_CHANNEL_CLOSED, old_sdp, 0);
    uint8_t data[5] = {0};
    sdp_client_packet_handler(L2CAP_DATA_PACKET, old_sdp, data, sizeof(data));
    channel_event(hid_host_packet_handler, L2CAP_EVENT_CHANNEL_OPENED, 0x333, 1);
    assert(!opened && hid_allocations == 1 && sdp_client_sdp_cid == fresh_sdp);
    assert(!hub_classic_hid_recover(cid, keyboard));
    assert(closed == 2 && !hid_allocations && !l2cap_channels);
}
static void test_close_before_sdp_open(void) {
    setup();
    uint16_t cid;
    assert(!hid_host_connect(keyboard, HID_PROTOCOL_MODE_REPORT, &cid));
    uint16_t channel = sdp_client_sdp_cid;
    assert(hub_classic_l2cap_reap(keyboard, sdp_client_packet_handler));
    channel_event(sdp_client_packet_handler, L2CAP_EVENT_CHANNEL_CLOSED, channel, 0);
    assert(opened == 1 && last_status && !hid_allocations && sdp_client_ready());
    channel_event(sdp_client_packet_handler, L2CAP_EVENT_CHANNEL_CLOSED, channel, 0);
    assert(opened == 1);
}
static void test_other_query_and_queued_hid(void) {
    setup();
    assert(!sdp_client_query_uuid16(other_query, peer, 0x1124));
    uint16_t other_channel = sdp_client_sdp_cid, cid;
    assert(!hid_host_connect(keyboard, HID_PROTOCOL_MODE_REPORT, &cid));
    assert(hid_host_get_connection_for_hid_cid(cid)->state == HID_HOST_W2_SEND_SDP_QUERY);
    assert(!hub_classic_hid_recover(cid, keyboard));
    assert(closed == 1 && !hid_allocations && !other_done);
    assert(sdp_client_sdp_cid == other_channel && sdp_parser_callback == other_query);
    assert(!channel_frees && l2cap_channels);
    assert(hub_classic_l2cap_reap(peer, sdp_client_packet_handler));
    channel_event(sdp_client_packet_handler, L2CAP_EVENT_CHANNEL_CLOSED, other_channel, 0);
    assert(other_done == 1 && sdp_client_ready() && !sdp_client_query_requests);
}
static void test_queued_peer_survives_retirement(void) {
    setup();
    uint16_t first, second, unrelated;
    assert(!hid_host_connect(keyboard, HID_PROTOCOL_MODE_REPORT, &first));
    hid_host_connection_t *old = hid_host_get_connection_for_hid_cid(first);
    old->hid_descriptor_len = 12;
    memset(descriptors, 0xaa, 12);
    assert(!hid_host_connect(peer, HID_PROTOCOL_MODE_REPORT, &second));
    assert(!l2cap_create_channel(other_query, keyboard, 3, 100, &unrelated));
    assert(!hub_classic_hid_recover(first, keyboard));
    hid_host_connection_t *next = hid_host_get_connection_for_hid_cid(second);
    assert(next && next->state == HID_HOST_W4_SDP_QUERY_RESULT);
    assert(next->hid_descriptor_offset == 0 && hid_host_sdp_context_control_cid == second);
    assert(!bd_addr_cmp(hub_sdp_remote, peer) && sdp_parser_callback == hid_host_handle_sdp_client_query_result);
    assert(channel_frees == 1 && closed == 1 && hid_allocations == 1);
    assert(hub_classic_l2cap_reap(keyboard, other_query));
    assert(channel_frees == 2);
    assert(!hub_classic_hid_recover(second, peer));
    assert(!l2cap_channels && !hid_allocations && sdp_client_ready() && closed == 2);
}
static void test_normal_channels_and_interrupt_failure(bool fail_interrupt) {
    setup();
    uint16_t cid;
    assert(!hid_host_connect(keyboard, HID_PROTOCOL_MODE_REPORT, &cid));
    hid_host_connection_t *connection = hid_host_get_connection_for_hid_cid(cid);
    connection->control_psm = BLUETOOTH_PSM_HID_CONTROL;
    connection->interrupt_psm = BLUETOOTH_PSM_HID_INTERRUPT;
    /* Fields normally assembled from SDP attribute bytes; exercise the real completion path. */
    assert(hub_classic_l2cap_reap(keyboard, sdp_client_packet_handler));
    sdp_client_state = QUERY_COMPLETE;
    channel_event(sdp_client_packet_handler, L2CAP_EVENT_CHANNEL_CLOSED, sdp_client_sdp_cid, 0);
    assert(sdp_client_ready() && connection->state == HID_HOST_W4_CONTROL_CONNECTION_ESTABLISHED);
    uint16_t control = connection->control_cid;
    if (fail_interrupt) create_status = BTSTACK_MEMORY_ALLOC_FAILED;
    channel_event(hid_host_packet_handler, L2CAP_EVENT_CHANNEL_OPENED, control, 0);
    if (fail_interrupt) {
        assert(opened == 1 && last_status == create_status && !hid_allocations);
        assert(hub_classic_l2cap_reap(keyboard, hid_host_packet_handler));
        assert(!l2cap_channels);
    } else {
        assert(!opened && connection->state == HID_HOST_W4_INTERRUPT_CONNECTION_ESTABLISHED);
        channel_event(hid_host_packet_handler, L2CAP_EVENT_CHANNEL_OPENED, connection->interrupt_cid, 0);
        assert(opened == 1 && !last_status && connection->state == HID_HOST_CONNECTION_ESTABLISHED);
        assert(hub_classic_hid_recover(cid, peer) == ERROR_CODE_INVALID_HCI_COMMAND_PARAMETERS);
        assert(!closed && hid_allocations == 1);
        assert(hub_classic_l2cap_reap(keyboard, hid_host_packet_handler));
        channel_event(hid_host_packet_handler, L2CAP_EVENT_CHANNEL_CLOSED, control, 0);
        assert(closed == 1 && !hid_allocations && !l2cap_channels);
    }
}
int main(void) {
    test_synchronous_failure();
    test_orphan_and_stale_events();
    test_close_before_sdp_open();
    test_other_query_and_queued_hid();
    test_queued_peer_survives_retirement();
    test_normal_channels_and_interrupt_failure(false);
    test_normal_channels_and_interrupt_failure(true);
    puts("Classic recovery: production HID/SDP lifecycle and scoped L2CAP reaping passed");
    return 0;
}
