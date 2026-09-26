include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/HubBtstackReconnect.cmake")

function(hub_generate_patched_hids original output)
    file(READ "${original}" source)
    string(REPLACE "\r\n" "\n" source "${source}")
    string(SHA256 source_hash "${source}")
    if(NOT source_hash STREQUAL "dda320acecfdbcab4764dc92ee674ad7df5a13e3aac14e58e855a5b638b0f714")
        message(FATAL_ERROR "BTstack hids_host.c differs from pinned revision; review the HIDS overlay (${source_hash})")
    endif()
    set(anchor "#define BTSTACK_FILE__ \"hids_host.c\"")
    hub_hci_find_once("${source}" "${anchor}" unused)
    string(REPLACE "${anchor}" "${anchor}\n#include \"hub_perf.h\"" source "${source}")

    hub_hci_extract("${source}"
        "        case HIDS_HOST_STATE_W2_ENABLE_INPUT_REPORTS:\n"
        "        case HIDS_HOST_W2_SEND_WRITE_REPORT:\n" old_start)
    file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../patches/btstack/hids_subscription_start.inc" replacement)
    string(REPLACE "\r\n" "\n" replacement "${replacement}")
    string(REPLACE "${old_start}" "${replacement}\n" source "${source}")

    set(anchor "            status = gatt_client_att_status_to_error_code(gatt_event_query_complete_get_att_status(packet));\n")
    hub_hci_find_once("${source}" "${anchor}" unused)
    set(check [=[
            if (client->state == HIDS_HOST_STATE_W4_INPUT_REPORTS_ENABLED){
                hub_perf_hids_cccd(client->con_handle, client->reports[client->report_index].value_handle,
                                   true, gatt_event_query_complete_get_att_status(packet));
            }
            if (status != ERROR_CODE_SUCCESS && client->state < HIDS_HOST_STATE_CONNECTED){
                hids_host_emit_connection_established(client, status);
                hids_host_finalize(client);
                return;
            }
]=])
    string(REPLACE "${anchor}" "${anchor}${check}" source "${source}")

    # A missing/malformed Report Reference must not bypass subscription setup.
    set(anchor "                case HIDS_HOST_STATE_W4_REPORT_FOUND:\n                    if (client->handle != 0){")
    hub_hci_find_once("${source}" "${anchor}" unused)
    set(check [=[
                    if (client->handle == 0 && !client->reports[client->report_index].boot_report){
                        hids_host_emit_connection_established(client, ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                        hids_host_finalize(client);
                        return;
                    }
]=])
    string(REPLACE "${anchor}"
        "                case HIDS_HOST_STATE_W4_REPORT_FOUND:\n${check}                    if (client->handle != 0){"
        source "${source}")
    hub_hci_extract("${source}"
        "                case HIDS_HOST_STATE_W4_REPORT_FOUND:\n                    if (client->handle == 0"
        "                case HIDS_HOST_STATE_W4_REPORT_ID_AND_TYPE:\n                    // go for next report\n" old_found)
    string(REPLACE "                    client->state = HIDS_HOST_STATE_CONNECTED;"
        "                    if (hids_host_report_notifications_init(client)) break;\n                    client->state = HIDS_HOST_STATE_CONNECTED;"
        new_found "${old_found}")
    string(REPLACE "${old_found}" "${new_found}" source "${source}")
    set(anchor "                case HIDS_HOST_STATE_W4_REPORT_ID_AND_TYPE:\n                    // go for next report\n")
    hub_hci_find_once("${source}" "${anchor}" unused)
    set(check [=[
                case HIDS_HOST_STATE_W4_REPORT_ID_AND_TYPE:
                    if (client->reports[client->report_index].report_type < HID_REPORT_TYPE_INPUT ||
                        client->reports[client->report_index].report_type > HID_REPORT_TYPE_FEATURE){
                        hids_host_emit_connection_established(client, ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                        hids_host_finalize(client);
                        return;
                    }
                    // go for next report
]=])
    string(REPLACE "${anchor}" "${check}" source "${source}")
    string(REGEX REPLACE
        "hids_host_emit_connection_established\\(client, ([A-Za-z_0-9]+)\\);\n[ ]*hids_host_finalize\\(client\\);"
        "hub_hids_connection_failed(client, \\1);" source "${source}")
    set(anchor "static void hids_host_emit_disconnected(")
    hub_hci_find_once("${source}" "${anchor}" unused)
    set(cleanup [=[
static void hub_hids_connection_failed(hids_host_t * client, uint8_t status){
    uint8_t cancel_status = gatt_client_remove_gatt_query(&client->gatt_query_request, client->con_handle);
    if (cancel_status != ERROR_CODE_SUCCESS && cancel_status != ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER){
        log_error("HIDS failed-query cleanup status 0x%02x", cancel_status);
    }
    hids_host_emit_connection_established(client, status);
    hids_host_finalize(client);
}

]=])
    string(REPLACE "${anchor}" "${cleanup}${anchor}" source "${source}")

    hub_hci_extract("${source}" "static void hids_host_send_next_query(void * context){"
        "static void hids_host_request_to_send_next_query(hids_host_t * client){" old_send)
    string(REPLACE "    uint8_t att_status;" "    uint8_t att_status = ERROR_CODE_SUCCESS;" new_send "${old_send}")
    set(anchor "\n    }\n}\n")
    hub_hci_find_once("${new_send}" "${anchor}" unused)
    set(check [=[
    }
    if (att_status != ERROR_CODE_SUCCESS && client->state < HIDS_HOST_STATE_CONNECTED){
        hub_hids_connection_failed(client, att_status);
    }
}
]=])
    string(REPLACE "${anchor}" "\n${check}" new_send "${new_send}")
    string(REPLACE "${old_send}" "${new_send}" source "${source}")

    set(anchor "    gatt_client_request_to_send_gatt_query(&client->gatt_query_request, client->con_handle);")
    hub_hci_find_once("${source}" "${anchor}" unused)
    set(check [=[
    uint8_t status = gatt_client_request_to_send_gatt_query(&client->gatt_query_request, client->con_handle);
    // Repeated discovery results can request the same already-queued callback.
    if (status != ERROR_CODE_SUCCESS && status != ERROR_CODE_COMMAND_DISALLOWED &&
        client->state < HIDS_HOST_STATE_CONNECTED){
        if (client->state == HIDS_HOST_STATE_W2_ENABLE_INPUT_REPORTS){
            hub_perf_hids_cccd(client->con_handle, client->reports[client->report_index].value_handle, false, status);
        }
        hub_hids_connection_failed(client, status);
    }
]=])
    string(REPLACE "${anchor}" "${check}" source "${source}")
    get_filename_component(directory "${output}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(CONFIGURE OUTPUT "${output}" CONTENT "${source}" @ONLY)
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${original}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../patches/btstack/hids_subscription_start.inc")
    endif()
endfunction()

function(hub_patch_btstack_hids)
    if(NOT PICO_SDK_VERSION_STRING STREQUAL "2.3.1" OR NOT TARGET pico_btstack_ble)
        message(FATAL_ERROR "The HIDS overlay requires Pico SDK 2.3.1 after pico_sdk_init()")
    endif()
    get_target_property(sources pico_btstack_ble INTERFACE_SOURCES)
    set(original "${PICO_BTSTACK_PATH}/src/ble/gatt-service/hids_host.c")
    set(matches 0)
    foreach(source IN LISTS sources)
        if(source STREQUAL original)
            math(EXPR matches "${matches} + 1")
        endif()
    endforeach()
    if(NOT matches EQUAL 1)
        message(FATAL_ERROR "Expected exactly one HIDS source in pico_btstack_ble; got ${matches}")
    endif()
    set(patched "${CMAKE_CURRENT_BINARY_DIR}/generated/btstack/hids_host.c")
    hub_generate_patched_hids("${original}" "${patched}")
    list(FIND sources "${original}" index)
    list(REMOVE_AT sources ${index})
    list(INSERT sources ${index} "${patched}")
    set_property(TARGET pico_btstack_ble PROPERTY INTERFACE_SOURCES "${sources}")
    message(STATUS "BTstack HIDS error propagation overlay: ${patched} (vendored SDK unchanged)")
endfunction()
