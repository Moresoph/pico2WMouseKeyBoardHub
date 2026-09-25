include_guard(GLOBAL)

# Source-local, experimental overlay for SDK 2.3.1 / BTstack eb0bb8b5.
# Never wrap L2CAP globally: outgoing host channels must retain their handlers.
function(hub_classic_replace text old replacement expected result)
    set(tail "${text}")
    set(count 0)
    string(LENGTH "${old}" length)
    while(TRUE)
        string(FIND "${tail}" "${old}" position)
        if(position LESS 0)
            break()
        endif()
        math(EXPR count "${count} + 1")
        math(EXPR after "${position} + ${length}")
        string(SUBSTRING "${tail}" ${after} -1 tail)
    endwhile()
    if(NOT count EQUAL expected)
        message(FATAL_ERROR "Pinned Classic mux anchor expected ${expected} occurrences, found ${count}: ${old}")
    endif()
    string(REPLACE "${old}" "${replacement}" patched "${text}")
    set(${result} "${patched}" PARENT_SCOPE)
endfunction()

function(hub_generate_classic_mux_source original output role)
    if(role STREQUAL "host")
        set(expected_hash "ac41fcdd3417d06e1f44e183f43c9190b1c7a6f72a88312aabdcd011bab355df")
        set(declines 5)
    elseif(role STREQUAL "device")
        set(expected_hash "5d1adf2e8233cac8262057fd6b23e32808891b6281adcacb562d43458d7462fc")
        set(declines 3)
    else()
        message(FATAL_ERROR "Unknown Classic mux role: ${role}")
    endif()
    file(READ "${original}" source)
    string(REPLACE "\r\n" "\n" source "${source}")
    string(SHA256 actual_hash "${source}")
    if(NOT actual_hash STREQUAL expected_hash)
        message(FATAL_ERROR
            "BTstack hid_${role}.c differs from pinned eb0bb8b5ea6d234ccb940313b47f7a5c3b4e20ec; "
            "review the Classic mux overlay before updating SDK (SHA256 ${actual_hash})")
    endif()
    hub_classic_replace("${source}" "#include \"l2cap.h\"\n"
        "#include \"l2cap.h\"\n#include \"hub_classic_mux.h\"\n" 1 source)
    hub_classic_replace("${source}" "l2cap_register_service("
        "hub_classic_mux_register_${role}(" 2 source)
    hub_classic_replace("${source}" "l2cap_decline_connection("
        "hub_classic_mux_decline(" ${declines} source)
    if(role STREQUAL "device")
        # CIDs are populated on acceptance, not on successful channel open.
        hub_classic_replace("${source}"
            "if (!connected_before && device->control_cid && device->interrupt_cid){"
            "if (!connected_before && hub_classic_mux_output_ready(device->con_handle)){"
            1 source)
        # Incoming open failure has no later CLOSED event. Release its singleton
        # CID here, preserving the other channel until its genuine close event.
        set(old_failure [=[                        if (device->incoming == 0){
                            // report error for outgoing connection
                            hid_device_emit_connected_event(device, status);
                        }
                        return;]=])
        set(new_failure [=[                        if (device->incoming == 0){
                            // report error for outgoing connection
                            hid_device_emit_connected_event(device, status);
                        }
                        local_cid = l2cap_event_channel_opened_get_local_cid(packet);
                        if (device->control_cid == local_cid) device->control_cid = 0;
                        if (device->interrupt_cid == local_cid) device->interrupt_cid = 0;
                        device->connected = 0;
                        if (!device->control_cid && !device->interrupt_cid){
                            device->incoming = 0;
                            device->con_handle = HCI_CON_HANDLE_INVALID;
                            device->cid = 0;
                            device->state = HID_DEVICE_IDLE;
                            device->user_request_can_send_now = 0;
                            hid_device_emit_event(device, HID_SUBEVENT_CONNECTION_CLOSED);
                        }
                        return;]=])
        hub_classic_replace("${source}" "${old_failure}" "${new_failure}" 1 source)
        # Disallow before the stock function can reset a live singleton.
        hub_classic_replace("${source}"
            "uint8_t hid_device_connect(bd_addr_t addr, uint16_t * hid_cid){\n"
            "uint8_t hid_device_connect(bd_addr_t addr, uint16_t * hid_cid){\n    (void)addr;\n    if (hid_cid) *hid_cid = 0;\n    return ERROR_CODE_COMMAND_DISALLOWED;\n"
            1 source)
    endif()
    get_filename_component(directory "${output}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(CONFIGURE OUTPUT "${output}" CONTENT "${source}" @ONLY)
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    endif()
endfunction()

function(hub_patch_btstack_classic_mux)
    if(NOT PICO_SDK_VERSION_STRING STREQUAL "2.3.1")
        message(FATAL_ERROR "The Classic mux overlay requires Pico SDK 2.3.1")
    endif()
    if(NOT TARGET pico_btstack_classic)
        message(FATAL_ERROR "Call hub_patch_btstack_classic_mux after pico_sdk_init()")
    endif()
    get_target_property(sources pico_btstack_classic INTERFACE_SOURCES)
    foreach(role host device)
        set(original "${PICO_BTSTACK_PATH}/src/classic/hid_${role}.c")
        set(matches 0)
        foreach(source IN LISTS sources)
            if(source STREQUAL original)
                math(EXPR matches "${matches} + 1")
            endif()
        endforeach()
        if(NOT matches EQUAL 1)
            message(FATAL_ERROR "Expected exactly one ${original} in pico_btstack_classic; got ${matches}")
        endif()
        set(patched "${CMAKE_CURRENT_BINARY_DIR}/generated/btstack/classic/hid_${role}.c")
        hub_generate_classic_mux_source("${original}" "${patched}" "${role}")
        list(FIND sources "${original}" index)
        list(REMOVE_AT sources ${index})
        list(INSERT sources ${index} "${patched}")
    endforeach()
    # Touch only Classic's two HID sources; preserve the separate HCI overlay.
    set_property(TARGET pico_btstack_classic PROPERTY INTERFACE_SOURCES "${sources}")
    message(STATUS "BTstack Classic host/device mux overlay enabled (vendored SDK unchanged)")
endfunction()
