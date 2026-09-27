static uint8_t hub_sdp_client_connect(bd_addr_t remote) {
    memcpy(hub_sdp_remote, remote, sizeof(bd_addr_t));
    sdp_client_sdp_cid = 0;
    uint8_t status = l2cap_create_channel(sdp_client_packet_handler, remote,
        BLUETOOTH_PSM_SDP, l2cap_max_mtu(), &sdp_client_sdp_cid);
    if (status) {
        sdp_client_state = INIT;
        sdp_client_sdp_cid = 0;
        sdp_parser_callback = NULL;
    }
    return status;
}

bool hub_classic_sdp_retire(const uint8_t address[6], btstack_packet_handler_t owner) {
    if (sdp_client_state == INIT || sdp_parser_callback != owner
        || bd_addr_cmp(hub_sdp_remote, address)) return true;
    if (!hub_classic_l2cap_reap(address, sdp_client_packet_handler)) return false;
    sdp_parser_callback = NULL;
    sdp_client_sdp_cid = 0;
    sdp_client_state = INIT;
    /* Registrations are persistent objects, not pointers into a retired HID context. */
    sdp_client_notify_callbacks();
    return true;
}
