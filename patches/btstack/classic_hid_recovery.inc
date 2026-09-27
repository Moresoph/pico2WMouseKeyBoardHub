uint8_t hub_classic_hid_recover(uint16_t cid, const uint8_t address[6]) {
    if (hci_connection_for_bd_addr_and_type(address, BD_ADDR_TYPE_ACL) != NULL)
        return ERROR_CODE_COMMAND_DISALLOWED;
    hid_host_connection_t *connection = hid_host_get_connection_for_hid_cid(cid);
    if (connection && bd_addr_cmp(connection->remote_addr, address))
        return ERROR_CODE_INVALID_HCI_COMMAND_PARAMETERS;
    if (!connection && hid_host_get_connection_for_bd_addr(address) != NULL)
        return ERROR_CODE_COMMAND_DISALLOWED;
    if (!hub_classic_l2cap_reap(address, hid_host_packet_handler))
        return ERROR_CODE_COMMAND_DISALLOWED;

    /* A queued SDP-ready callback must not start this cancelled connection. */
    if (connection) connection->state = HID_HOST_IDLE;
    if (hid_host_sdp_context_control_cid == cid) {
        hid_host_sdp_context_control_cid = 0;
        if (!hub_classic_sdp_retire(address, hid_host_handle_sdp_client_query_result))
            return ERROR_CODE_COMMAND_DISALLOWED;
    }
    connection = hid_host_get_connection_for_hid_cid(cid);
    if (connection) {
        connection->control_cid = 0;
        connection->interrupt_cid = 0;
        hid_descriptor_storage_delete(connection);
        /* Remove before notifying the application; it may start a new attempt. */
        hid_host_finalize_connection(connection);
        uint8_t event[5] = {HCI_EVENT_HID_META, 3, HID_SUBEVENT_CONNECTION_CLOSED, 0, 0};
        little_endian_store_16(event, 3, cid);
        hid_host_callback(HCI_EVENT_PACKET, cid, event, sizeof(event));
    }
    return ERROR_CODE_SUCCESS;
}
