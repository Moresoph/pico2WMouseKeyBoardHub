        case HIDS_HOST_STATE_W2_ENABLE_INPUT_REPORTS:
            client->state = HIDS_HOST_STATE_W4_INPUT_REPORTS_ENABLED;
            characteristic.value_handle = client->reports[client->report_index].value_handle;
            characteristic.end_handle = client->reports[client->report_index].end_handle;
            characteristic.properties = client->reports[client->report_index].properties;
            att_status = gatt_client_write_client_characteristic_configuration(
                &handle_gatt_client_event, client->con_handle, &characteristic,
                GATT_CLIENT_CHARACTERISTICS_CONFIGURATION_NOTIFICATION);
            hub_perf_hids_cccd(client->con_handle, characteristic.value_handle, false, att_status);
            if (att_status == ERROR_CODE_SUCCESS){
                gatt_client_listen_for_characteristic_value_updates(
                    &client->reports[client->report_index].notification_listener,
                    &hids_host_handle_notification_event, client->con_handle, &characteristic);
            }
            break;
