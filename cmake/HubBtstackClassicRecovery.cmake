include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/HubBtstackReconnect.cmake")

function(hub_recovery_replace text old replacement result)
    hub_hci_find_once("${text}" "${old}" unused)
    string(REPLACE "${old}" "${replacement}" text "${text}")
    set(${result} "${text}" PARENT_SCOPE)
endfunction()

function(hub_recover_classic_text source role result)
    set(patches "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../patches/btstack")
    file(READ "${patches}/classic_${role}_recovery.inc" recovery)
    string(REPLACE "\r\n" "\n" recovery "${recovery}")
    set(source "#include \"hub_classic_recovery.h\"\n#include \"hci.h\"\n${source}")
    if(role STREQUAL "hid")
        hub_recovery_replace("${source}"
            "hid_host_get_connection_for_bd_addr(bd_addr_t addr)"
            "hid_host_get_connection_for_bd_addr(const bd_addr_t addr)" source)
        set(old "        sdp_client_query_uuid16(&hid_host_handle_sdp_client_query_result, (uint8_t *) connection->remote_addr, BLUETOOTH_SERVICE_CLASS_HUMAN_INTERFACE_DEVICE_SERVICE);")
        set(new [=[        uint16_t cid = connection->hid_cid;
        uint8_t status = sdp_client_query_uuid16(&hid_host_handle_sdp_client_query_result,
            connection->remote_addr, BLUETOOTH_SERVICE_CLASS_HUMAN_INTERFACE_DEVICE_SERVICE);
        if (status && hid_host_get_connection_for_hid_cid(cid) == connection) {
            hid_host_sdp_context_control_cid = 0;
            hid_descriptor_storage_delete(connection);
            hid_emit_connected_event(connection, status);
            if (hid_host_get_connection_for_hid_cid(cid) == connection)
                hid_host_finalize_connection(connection);
        }]=])
        hub_recovery_replace("${source}" "${old}" "${new}" source)
        set(old [=[                                        log_info("Connecting to HID Interrupt failed: 0x%02x", status);
                                        hid_emit_connected_event(connection, status);
                                        break;]=])
        set(new [=[                                        log_info("Connecting to HID Interrupt failed: 0x%02x", status);
                                        uint16_t cid = connection->hid_cid;
                                        hid_descriptor_storage_delete(connection);
                                        hid_emit_connected_event(connection, status);
                                        if (hid_host_get_connection_for_hid_cid(cid) == connection)
                                            hid_host_finalize_connection(connection);
                                        break;]=])
        hub_recovery_replace("${source}" "${old}" "${new}" source)
        set(old [=[                    connection = hid_host_get_connection_for_bd_addr(address);
                    if (!connection){
                        log_error("Connection does not exist %s", bd_addr_to_str(address));]=])
        set(new [=[                    connection = hid_host_get_connection_for_l2cap_cid(
                        l2cap_event_channel_opened_get_local_cid(packet));
                    if (!connection || bd_addr_cmp(connection->remote_addr, address)){
                        log_error("Stale HID channel open for %s", bd_addr_to_str(address));]=])
        hub_recovery_replace("${source}" "${old}" "${new}" source)
    elseif(role STREQUAL "sdp")
        hub_recovery_replace("${source}" "static uint16_t  sdp_client_mtu;"
            "static bd_addr_t hub_sdp_remote;\nstatic uint8_t hub_sdp_client_connect(bd_addr_t remote);\nstatic uint16_t  sdp_client_mtu;" source)
        set(old "    return l2cap_create_channel(sdp_client_packet_handler, remote, BLUETOOTH_PSM_SDP, l2cap_max_mtu(), NULL);")
        # All three query entry points share transport ownership and failure handling.
        string(REPLACE "${old}" "    return hub_sdp_client_connect(remote);" source "${source}")
        string(REPLACE
            "    l2cap_create_channel(sdp_client_packet_handler, remote, BLUETOOTH_PSM_SDP, l2cap_max_mtu(), NULL);\n    return 0;"
            "    return hub_sdp_client_connect(remote);" source "${source}")
        if(source MATCHES "l2cap_max_mtu\\(\\), NULL\\)")
            message(FATAL_ERROR "Unpatched SDP channel creation")
        endif()
        set(old [=[void sdp_client_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
]=])
        set(new [=[void sdp_client_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    if (sdp_client_state == INIT || channel != sdp_client_sdp_cid) return;
]=])
        hub_recovery_replace("${source}" "${old}" "${new}" source)
        hub_hci_extract("${source}" "void sdp_parser_handle_done(uint8_t status){"
            "// SDP Client\n" old_done)
        set(new_done [=[void sdp_parser_handle_done(uint8_t status){
    if (sdp_client_state == INIT || sdp_parser_callback == NULL) return;
    btstack_packet_handler_t callback = sdp_parser_callback;
    sdp_parser_callback = NULL;
    sdp_client_sdp_cid = 0;
    sdp_client_state = INIT;
    uint8_t event[] = {SDP_EVENT_QUERY_COMPLETE, 1, status};
    callback(HCI_EVENT_PACKET, 0, event, sizeof(event));
    sdp_client_notify_callbacks();
}

]=])
        hub_recovery_replace("${source}" "${old_done}" "${new_done}" source)
    endif()
    set(${result} "${source}\n${recovery}" PARENT_SCOPE)
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${patches}/classic_${role}_recovery.inc")
    endif()
endfunction()

function(hub_generate_classic_recovery original output role)
    if(role STREQUAL "hid")
        set(expected "ac41fcdd3417d06e1f44e183f43c9190b1c7a6f72a88312aabdcd011bab355df")
    elseif(role STREQUAL "sdp")
        set(expected "0a4718fa7c57159004078ddf640c6c67997b35506811b5fb683c2e6747dbfd46")
    elseif(role STREQUAL "l2cap")
        set(expected "4c95a41e650f33a65e3f729cfe02a3d70255d43db1eaf3bdb32300f8286b9455")
    else()
        message(FATAL_ERROR "Unknown Classic recovery role ${role}")
    endif()
    file(READ "${original}" source)
    string(REPLACE "\r\n" "\n" source "${source}")
    string(SHA256 actual "${source}")
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "Classic recovery source differs from pinned BTstack: ${original} (${actual})")
    endif()
    hub_recover_classic_text("${source}" "${role}" source)
    get_filename_component(directory "${output}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(CONFIGURE OUTPUT "${output}" CONTENT "${source}" @ONLY)
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    endif()
endfunction()

function(hub_patch_btstack_classic_recovery)
    if(NOT PICO_SDK_VERSION_STRING STREQUAL "2.3.1")
        message(FATAL_ERROR "Classic recovery requires Pico SDK 2.3.1")
    endif()
    foreach(role l2cap sdp hid)
        if(role STREQUAL "l2cap")
            set(target pico_btstack_base)
            set(path "l2cap.c")
        elseif(role STREQUAL "sdp")
            set(target pico_btstack_classic)
            set(path "classic/sdp_client.c")
        else()
            if(HUB_CLASSIC_OUTPUT_TEST)
                continue() # The mux generator composes the HID recovery overlay.
            endif()
            set(target pico_btstack_classic)
            set(path "classic/hid_host.c")
        endif()
        get_target_property(sources ${target} INTERFACE_SOURCES)
        set(original "${PICO_BTSTACK_PATH}/src/${path}")
        set(matches 0)
        foreach(source IN LISTS sources)
            if(source STREQUAL original)
                math(EXPR matches "${matches} + 1")
            endif()
        endforeach()
        if(NOT matches EQUAL 1)
            message(FATAL_ERROR "Expected exactly one ${original} in ${target}")
        endif()
        set(output "${CMAKE_CURRENT_BINARY_DIR}/generated/btstack/${path}")
        hub_generate_classic_recovery("${original}" "${output}" "${role}")
        list(FIND sources "${original}" index)
        list(REMOVE_AT sources ${index})
        list(INSERT sources ${index} "${output}")
        set_property(TARGET ${target} PROPERTY INTERFACE_SOURCES "${sources}")
    endforeach()
endfunction()
