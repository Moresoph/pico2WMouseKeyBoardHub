include_guard(GLOBAL)

function(hub_usb_extract source begin end output)
    string(FIND "${source}" "${begin}" first)
    string(FIND "${source}" "${end}" last)
    if(first LESS 0 OR last LESS 0 OR NOT last GREATER first)
        message(FATAL_ERROR "Pinned USB overlay section missing or out of order")
    endif()
    math(EXPR length "${last} - ${first}")
    string(SUBSTRING "${source}" ${first} ${length} text)
    set(${output} "${text}" PARENT_SCOPE)
endfunction()

function(hub_generate_usb_hcd original output)
    file(READ "${original}" source)
    string(REPLACE "\r\n" "\n" source "${source}")
    string(SHA256 hash "${source}")
    if(NOT hash STREQUAL "635b3573f6de2b3fd2d44076a2cde404f0bd157de3b4d12180038218837a25b0")
        message(FATAL_ERROR "TinyUSB RP HCD differs from pinned 86ad6e5; review overlay (SHA256 ${hash})")
    endif()
    set(diagnostics "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../patches/tinyusb/host_diagnostics.inc")
    file(READ "${diagnostics}" extra)
    string(REPLACE "\r\n" "\n" extra "${extra}")
    string(REPLACE "#define epx (ep_pool[0])"
        "#define epx (ep_pool[0])\n${extra}" source "${source}")
    set(recovery "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../patches/tinyusb/host_transfer_recovery.inc")
    file(READ "${recovery}" extra)
    string(REPLACE "\r\n" "\n" extra "${extra}")
    set(irq_anchor "static void __tusb_irq_path_func(hcd_rp2040_irq)(void)")
    string(REPLACE "${irq_anchor}" "${extra}\n${irq_anchor}" source "${source}")
    string(REPLACE
        "SIE_CTRL_BASE = USB_SIE_CTRL_SOF_EN_BITS      | USB_SIE_CTRL_KEEP_ALIVE_EN_BITS |\n                  USB_SIE_CTRL_PULLDOWN_EN_BITS | USB_SIE_CTRL_EP0_INT_1BUF_BITS"
        "SIE_CTRL_BASE = USB_SIE_CTRL_PULLDOWN_EN_BITS | USB_SIE_CTRL_EP0_INT_1BUF_BITS"
        source "${source}")
    string(REPLACE "      hcd_event_device_attach(RHPORT_NATIVE, true);"
        "      usb_hw->sie_ctrl = hub_usb_hcd_bus_flags();\n      hcd_event_device_attach(RHPORT_NATIVE, true);"
        source "${source}")
    string(REPLACE "USB_SIE_CTRL_START_TRANS_BITS | SIE_CTRL_BASE |"
        "USB_SIE_CTRL_START_TRANS_BITS | hub_usb_hcd_bus_flags() |" source "${source}")
    string(REPLACE "uint32_t const flags = SIE_CTRL_BASE | USB_SIE_CTRL_SEND_SETUP_BITS"
        "uint32_t const flags = hub_usb_hcd_bus_flags() | USB_SIE_CTRL_SEND_SETUP_BITS" source "${source}")
    string(REPLACE "  rp2040_usb_init();"
        [=[  rp2040_usb_init();
#if defined(PICO_RP2350) && PICO_RP2350
  /* Include Hub repeater delays in the hardware's turnaround timeout. */
  usb_hw_set->linestate_tuning = USB_LINESTATE_TUNING_MULTI_HUB_FIX_BITS;
#endif]=] source "${source}")
    string(REPLACE "  usb_hw->dev_addr_ctrl = dev_addr;\n\n  // Set pre"
        "  usb_hw->dev_addr_ctrl = dev_addr;\n  hub_usb_wire_setup(dev_addr, setup_packet);\n\n  // Set pre"
        source "${source}")

    hub_usb_extract("${source}" "  if ( status & USB_INTS_STALL_BITS )"
        "  if ( status & USB_INTS_BUFF_STATUS_BITS )" old_stall)
    set(error_handler [=[
  if (status & (USB_INTS_STALL_BITS | USB_INTS_ERROR_RX_TIMEOUT_BITS)) {
    handled |= status & (USB_INTS_STALL_BITS | USB_INTS_ERROR_RX_TIMEOUT_BITS);
    if (status & USB_INTS_ERROR_RX_TIMEOUT_BITS) {
      hub_usb_hcd_note_rx_timeout();
      usb_hw_clear->sie_status = USB_SIE_STATUS_RX_TIMEOUT_BITS;
    }
    if (status & USB_INTS_STALL_BITS) {
      usb_hw_clear->sie_status = USB_SIE_STATUS_STALL_REC_BITS;
    }
    hub_usb_hcd_fail_epx((status & USB_INTS_STALL_BITS) ? XFER_RESULT_STALLED : XFER_RESULT_FAILED);
    status &= ~USB_INTS_TRANS_COMPLETE_BITS;
  }

]=])
    string(REPLACE "${old_stall}" "${error_handler}" source "${source}")
    hub_usb_extract("${source}" "  if ( status & USB_INTS_ERROR_RX_TIMEOUT_BITS )"
        "  if ( status & USB_INTS_ERROR_DATA_SEQ_BITS )" old_timeout)
    string(REPLACE "${old_timeout}" "" source "${source}")
    string(REPLACE "  memset(&ep_pool, 0, sizeof(ep_pool));"
        "  memset(&ep_pool, 0, sizeof(ep_pool));\n  hub_rx_timeouts = 0;\n  hub_root_connection_irq = 0;\n  hub_root_reset_active = false;"
        source "${source}")
    string(REPLACE "  // TODO: Nothing to do here yet. Perhaps need to reset some state?"
        [=[  uint32_t irq = save_and_disable_interrupts();
  if (epx.active || hub_root_reset_active) {
    restore_interrupts(irq);
    TU_LOG1("[usb-hcd] root reset rejected: control transfer or reset still active\n");
    return;
  }
  /* Reset changes SPEED too; these are not physical detach/attach events. */
  hub_root_connection_irq = usb_hw->inte & USB_INTE_HOST_CONN_DIS_BITS;
  usb_hw_clear->inte = USB_INTE_HOST_CONN_DIS_BITS;
  hub_root_reset_active = true;
  usb_hw->sie_ctrl = hub_usb_hcd_bus_flags() | USB_SIE_CTRL_RESET_BUS_BITS;
  restore_interrupts(irq);
  TU_LOG1("[usb-hcd] root bus reset requested\n");]=] source "${source}")
    string(REPLACE "void hcd_port_reset_end(uint8_t rhport)\n{\n  (void) rhport;\n}"
        [=[void hcd_port_reset_end(uint8_t rhport)
{
  (void) rhport;
  uint32_t irq = save_and_disable_interrupts();
  if (!hub_root_reset_active) {
    restore_interrupts(irq);
    return;
  }
  bool pending = (usb_hw->sie_ctrl & USB_SIE_CTRL_RESET_BUS_BITS) != 0;
  usb_hw_clear->sie_status = USB_SIE_STATUS_SPEED_BITS;
  usb_hw_set->inte = hub_root_connection_irq;
  hub_root_reset_active = false;
  restore_interrupts(irq);
  TU_LOG1("[usb-hcd] root reset completion: pending=%u\n",
          (unsigned)pending);
}]=] source "${source}")
    set(source "#include \"hardware/sync.h\"\n#include \"hub_usb_host.h\"\n#include \"hub_usb_wire.h\"\n${source}")
    get_filename_component(directory "${output}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(CONFIGURE OUTPUT "${output}" CONTENT "${source}" @ONLY)
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${original}" "${diagnostics}" "${recovery}")
    endif()
endfunction()

function(hub_generate_usb_hid original output)
    file(READ "${original}" source)
    string(REPLACE "\r\n" "\n" source "${source}")
    string(SHA256 hash "${source}")
    if(NOT hash STREQUAL "c4f1125c2235dc870b280343df5d4c088824c6d40430ea65cd21cf56f84ddea9")
        message(FATAL_ERROR "TinyUSB HID Host differs from pinned 86ad6e5 (SHA256 ${hash})")
    endif()
    string(REPLACE
        "    tuh_hid_report_received_cb(daddr, idx, epbuf->epin, (uint16_t) xferred_bytes);"
        "    tuh_hid_report_received_cb(daddr, idx, epbuf->epin,\n                               result == XFER_RESULT_SUCCESS ? (uint16_t) xferred_bytes : 0);"
        source "${source}")
    get_filename_component(directory "${output}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(CONFIGURE OUTPUT "${output}" CONTENT "${source}" @ONLY)
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
    endif()
endfunction()

function(hub_generate_usb_usbh original output)
    file(READ "${original}" source)
    string(REPLACE "\r\n" "\n" source "${source}")
    string(SHA256 hash "${source}")
    if(NOT hash STREQUAL "eeb692225f34e3ea9fbd195e0b4b80a5fb5f1d36b092a42a78d6b88a37574ec0")
        message(FATAL_ERROR "TinyUSB USBH differs from pinned 86ad6e5 (SHA256 ${hash})")
    endif()
    # Relocation must not resolve to BTstack's generated hub.h GATT database.
    string(REPLACE "#include \"hub.h\"" "#include \"host/hub.h\"" source "${source}")
    set(recovery "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../patches/tinyusb/host_enum_recovery.inc")
    set(prepare "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../patches/tinyusb/host_enum_prepare.inc")
    file(READ "${recovery}" extra)
    file(READ "${prepare}" prepare_source)
    string(APPEND extra "\n${prepare_source}")
    string(REPLACE "\r\n" "\n" extra "${extra}")
    string(REPLACE "static bool enum_new_device(hcd_event_t* event);"
        "static bool hub_enum_cleanup_pending(void);\nstatic bool hub_enum_cleanup_blocked(void);\nstatic bool hub_enum_prepare_active(void);\nstatic void hub_enum_parent_removed(uint8_t address);\nstatic bool enum_new_device(hcd_event_t* event);"
        source "${source}")
    string(REPLACE "static void enum_full_complete(void);"
        "static void enum_full_complete(void);\n${extra}" source "${source}")
    string(REPLACE "    if (!retry) {\n      enum_full_complete();"
        "    if (!retry) {\n      failed_count = 0;\n      hub_enum_failed();" source "${source}")
    string(REPLACE "      case HCD_EVENT_DEVICE_ATTACH:\n"
        [=[      case HCD_EVENT_DEVICE_ATTACH:
        if (hub_enum_cleanup_blocked()) {
          TU_LOG1("[usb-enum] attach blocked: reconnect Hub after cleanup error\n");
          break;
        }
        if (hub_enum_cleanup_pending()) {
          if (hub_enum_prepare_active() && event.rhport == _dev0.rhport &&
              event.connection.hub_addr == _dev0.hub_addr && event.connection.hub_port == _dev0.hub_port) {
            TU_LOG1("[usb-enum] duplicate attach ignored while checking port stability\n");
            break;
          }
          queue_event(&event, in_isr);
          return;
        }
]=] source "${source}")
    string(REPLACE "        hcd_device_close(rhport, daddr);"
        "        hub_enum_parent_removed(daddr);\n        hcd_device_close(rhport, daddr);"
        source "${source}")
    string(REPLACE
        [=[    // connected via external hub
    // wait until device connection is stable TODO non blocking
    tusb_time_delay_ms_api(ENUM_DEBOUNCING_DELAY_MS);

    // ENUM_HUB_GET_STATUS
    TU_ASSERT(hub_port_get_status(_dev0.hub_addr, _dev0.hub_port, _usbh_epbuf.ctrl,
                                  process_enumeration, ENUM_HUB_CLEAR_RESET_1));]=]
        "    hub_enum_prepare_begin();" source "${source}")
    set(source "#include <stdio.h>\n#include \"pico/time.h\"\n#include \"hub_usb_host.h\"\n${source}")
    get_filename_component(directory "${output}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(CONFIGURE OUTPUT "${output}" CONTENT "${source}" @ONLY)
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}" "${recovery}" "${prepare}")
    endif()
endfunction()

function(hub_generate_usb_hub original output)
    file(READ "${original}" source)
    string(REPLACE "\r\n" "\n" source "${source}")
    string(SHA256 hash "${source}")
    if(NOT hash STREQUAL "8c0ffb78d8906c42e0b875e707ade041d36d31b73f5f41cb46a545d01f34e846")
        message(FATAL_ERROR "TinyUSB Hub differs from pinned 86ad6e5 (SHA256 ${hash})")
    endif()
    # Relocation must not pick BTstack's generated GATT hub.h.
    foreach(header hcd usbh usbh_pvt hub)
        string(REPLACE "#include \"${header}.h\"" "#include \"host/${header}.h\"" source "${source}")
    endforeach()
    string(REPLACE "  uint8_t port_count;"
        "  uint8_t port_count;\n  uint16_t power_good_ms;\n  uint32_t power_started_ms;\n  bool power_waiting;"
        source "${source}")
    string(REPLACE "  p_hub->port_count = desc_hub->bNbrPorts;"
        [=[  if (!desc_hub->bNbrPorts || desc_hub->bNbrPorts > 7) {
    hub_usb_enum_block(daddr, 0, "Hub port count unsupported by one-byte status bitmap");
    return;
  }
  p_hub->port_count = desc_hub->bNbrPorts;
  p_hub->power_good_ms = (uint16_t)desc_hub->bPwrOn2PwrGood * 2u;]=]
        source "${source}")
    string(REPLACE "  // only use number of ports in hub descriptor"
        [=[  if (xfer->actual_len < sizeof(descriptor_hub_desc_t)) {
    hub_usb_enum_block(daddr, 0, "Hub descriptor read short");
    return;
  }
  // only use number of ports in hub descriptor]=] source "${source}")
    string(REPLACE "  TU_ASSERT(XFER_RESULT_SUCCESS == xfer->result, );"
        [=[  if (xfer->result != XFER_RESULT_SUCCESS) {
    hub_usb_enum_block(xfer->daddr, 0, "Hub configuration transfer failed");
    return;
  }]=] source "${source}")
    string(REPLACE
        "hub_port_set_feature(daddr, hub_port, HUB_FEATURE_PORT_POWER, config_port_power_complete, 0);"
        [=[if (!hub_port_set_feature(daddr, hub_port, HUB_FEATURE_PORT_POWER, config_port_power_complete, 0)) {
    hub_usb_enum_block(daddr, 0, "Hub power submission failed");
  }]=] source "${source}")
    string(REPLACE "    tu_memclr(p_hub, sizeof( hub_interface_t));"
        "    if (p_hub->power_waiting) hub_usb_enum_cancel_power_wait();\n    tu_memclr(p_hub, sizeof( hub_interface_t));"
        source "${source}")
    string(REPLACE
        [=[    // All ports are power -> queue notification status endpoint and
    // complete the SET CONFIGURATION
    TU_ASSERT( usbh_edpt_xfer(daddr, p_hub->ep_in, &p_hub->status_change, 1), );

    usbh_driver_set_config_complete(daddr, p_hub->itf_num);]=]
        [=[    p_hub->power_started_ms = (uint32_t)(time_us_64() / 1000u);
    p_hub->power_waiting = true;
    TU_LOG1("[usb-hub] hub=%u power-good wait=%u ms\n", daddr, p_hub->power_good_ms);]=]
        source "${source}")
    string(REPLACE
        "    // Reset port if attach event\n    hub_port_reset(daddr, port_num, connection_port_reset_complete, 0);"
        "    // USBH owns debounce/reset after accepting this enumeration.\n    connection_port_reset_complete(xfer);"
        source "${source}")
    string(REPLACE "connection_port_reset_complete" "connection_attach_complete" source "${source}")
    set(power "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../patches/tinyusb/host_hub_power.inc")
    file(READ "${power}" extra)
    string(REPLACE "\r\n" "\n" extra "${extra}")
    string(REPLACE "static void config_set_port_power (tuh_xfer_t* xfer);"
        "${extra}\nstatic void config_set_port_power (tuh_xfer_t* xfer);" source "${source}")
    set(source "#include \"pico/time.h\"\n#include \"hub_usb_host.h\"\n${source}")
    get_filename_component(directory "${output}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(CONFIGURE OUTPUT "${output}" CONTENT "${source}" @ONLY)
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}" "${power}")
    endif()
endfunction()

function(hub_patch_tinyusb_host)
    get_target_property(sources tinyusb_host_base INTERFACE_SOURCES)
    set(original "${PICO_TINYUSB_PATH}/src/portable/raspberrypi/rp2040/hcd_rp2040.c")
    set(matches 0)
    foreach(source IN LISTS sources)
        if(source STREQUAL original)
            math(EXPR matches "${matches} + 1")
        endif()
    endforeach()
    if(NOT matches EQUAL 1)
        message(FATAL_ERROR "Expected one native RP HCD in tinyusb_host_base; got ${matches}")
    endif()
    set(patched "${CMAKE_CURRENT_BINARY_DIR}/generated/tinyusb/hcd_rp2040.c")
    hub_generate_usb_hcd("${original}" "${patched}")
    list(FIND sources "${original}" index)
    list(REMOVE_AT sources ${index})
    list(INSERT sources ${index} "${patched}")
    set(hid_original "${PICO_TINYUSB_PATH}/src/class/hid/hid_host.c")
    set(hid_patched "${CMAKE_CURRENT_BINARY_DIR}/generated/tinyusb/hid_host.c")
    list(FIND sources "${hid_original}" hid_index)
    if(hid_index LESS 0)
        message(FATAL_ERROR "TinyUSB HID Host source missing")
    endif()
    hub_generate_usb_hid("${hid_original}" "${hid_patched}")
    list(REMOVE_AT sources ${hid_index})
    list(INSERT sources ${hid_index} "${hid_patched}")
    set(usbh_original "${PICO_TINYUSB_PATH}/src/host/usbh.c")
    set(usbh_patched "${CMAKE_CURRENT_BINARY_DIR}/generated/tinyusb/usbh.c")
    list(FIND sources "${usbh_original}" usbh_index)
    if(usbh_index LESS 0)
        message(FATAL_ERROR "TinyUSB USBH source missing")
    endif()
    hub_generate_usb_usbh("${usbh_original}" "${usbh_patched}")
    list(REMOVE_AT sources ${usbh_index})
    list(INSERT sources ${usbh_index} "${usbh_patched}")
    set(hub_original "${PICO_TINYUSB_PATH}/src/host/hub.c")
    set(hub_patched "${CMAKE_CURRENT_BINARY_DIR}/generated/tinyusb/hub.c")
    list(FIND sources "${hub_original}" hub_index)
    if(hub_index LESS 0)
        message(FATAL_ERROR "TinyUSB Hub source missing")
    endif()
    hub_generate_usb_hub("${hub_original}" "${hub_patched}")
    list(REMOVE_AT sources ${hub_index})
    list(INSERT sources ${hub_index} "${hub_patched}")
    set_property(TARGET tinyusb_host_base PROPERTY INTERFACE_SOURCES "${sources}")
    target_include_directories(tinyusb_host_base INTERFACE
        "${PICO_TINYUSB_PATH}/src/portable/raspberrypi/rp2040" "${PICO_TINYUSB_PATH}/src/class/hid")
    message(STATUS "TinyUSB Host transfer/recovery overlay: ${patched} (vendored SDK unchanged)")
endfunction()
