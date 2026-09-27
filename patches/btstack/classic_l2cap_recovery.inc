bool hub_classic_l2cap_reap(const uint8_t address[6], btstack_packet_handler_t owner) {
    /* No live or pending ACL may retain ownership of these channels. */
    if (hci_connection_for_bd_addr_and_type(address, BD_ADDR_TYPE_ACL) != NULL) return false;
    btstack_linked_list_iterator_t it;
    btstack_linked_list_iterator_init(&it, &l2cap_channels);
    while (btstack_linked_list_iterator_has_next(&it)) {
        l2cap_channel_t *channel = (l2cap_channel_t *)btstack_linked_list_iterator_next(&it);
        if (channel->channel_type != L2CAP_CHANNEL_TYPE_CLASSIC
            || channel->packet_handler != owner || bd_addr_cmp(channel->address, address)) continue;
        btstack_linked_list_iterator_remove(&it);
        /* The owning profile completes cancellation after its callbacks are detached. */
        l2cap_free_channel_entry(channel);
    }
    return true;
}
