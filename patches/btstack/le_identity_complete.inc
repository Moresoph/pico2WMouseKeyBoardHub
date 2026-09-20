        // CYW may report the same identity as a plain public/static random address.
        // Only reuse the pending central context, never another established link.
        if ((conn == NULL) &&
            (hci_event_le_meta_get_subevent_code(hci_event) != HCI_SUBEVENT_LE_CONNECTION_COMPLETE)){
            hci_connection_t * outgoing = gap_get_outgoing_le_connection();
            if ((outgoing != NULL) &&
                (outgoing->role == HCI_ROLE_MASTER) &&
                (outgoing->state == SENT_CREATE_CONNECTION) &&
                hci_is_le_identity_address_type(outgoing->address_type) &&
                (addr_type == (outgoing->address_type & 1)) &&
                (memcmp(outgoing->address, addr, sizeof(bd_addr_t)) == 0)){
                log_error("Hub HCI: reused pending LE identity context (type %u -> %u)",
                          outgoing->address_type, addr_type);
                conn = outgoing;
            }
        }
