// Error events need not contain a valid peer address. Never free the
        // address lookup result: it may be an unrelated established connection.
        bool connection_was_cancelled = status == ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
        bool cancelled_by_user = hci_stack->le_connecting_request == LE_CONNECTING_IDLE;
        bool direct_attempt = hci_stack->le_connecting_request == LE_CONNECTING_DIRECT;
        conn = gap_get_outgoing_le_connection();
        if ((conn != NULL) && (conn->role != HCI_ROLE_MASTER)){
            conn = NULL;
        }
        hci_stack->le_connecting_state = LE_CONNECTING_IDLE;

        // An internal cancel pauses initiation while resolving/filter lists change.
        // Keep the request and pending context so hci_run can intentionally restart.
        if (connection_was_cancelled && direct_attempt){
            if (conn != NULL){
                conn->state = SEND_CREATE_CONNECTION;
            }
        } else {
            if (direct_attempt){
                hci_stack->le_connecting_request = LE_CONNECTING_IDLE;
            }
            if ((conn != NULL) && (direct_attempt || cancelled_by_user)){
                if (!connection_was_cancelled){
                    log_error("Hub HCI: released failed direct LE attempt (status 0x%02x)", status);
                }
                btstack_linked_list_remove(&hci_stack->connections, (btstack_linked_item_t *) conn);
                btstack_memory_hci_connection_free(conn);
            }
        }
