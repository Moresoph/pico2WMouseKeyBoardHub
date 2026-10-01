cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED SDK_PATH OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "Pass SDK_PATH and OUTPUT_DIR")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/HubTinyUsbHost.cmake")
set(original "${SDK_PATH}/lib/tinyusb/src/portable/raspberrypi/rp2040/hcd_rp2040.c")
hub_generate_usb_hcd("${original}" "${OUTPUT_DIR}/hcd_rp2040.c")
file(READ "${OUTPUT_DIR}/hcd_rp2040.c" source)
hub_usb_extract("${source}" "static void __tusb_irq_path_func(hcd_rp2040_irq)(void)"
    "void __tusb_irq_path_func(hcd_int_handler)" irq)
hub_usb_extract("${source}" "enum {\n  SIE_CTRL_BASE" "static struct hw_endpoint *get_dev_ep" flags)
hub_usb_extract("${source}" "static uint32_t hub_root_connection_irq;"
    "static void __tusb_irq_path_func(hcd_rp2040_irq)(void)" recovery)
hub_usb_extract("${source}" "void hcd_port_reset(uint8_t rhport)"
    "bool hcd_port_connect_status(uint8_t rhport)" reset)
hub_usb_extract("${source}" "bool hcd_init(" "bool hcd_deinit(" init)
hub_usb_extract("${source}" "bool hcd_setup_send(" "bool hcd_edpt_clear_stall(" setup)
file(WRITE "${OUTPUT_DIR}/usb_hcd_irq.inc" "${flags}\n${recovery}\n${irq}\n${init}\n${reset}\n${setup}")
hub_generate_usb_hid("${SDK_PATH}/lib/tinyusb/src/class/hid/hid_host.c" "${OUTPUT_DIR}/hid_host.c")
file(READ "${OUTPUT_DIR}/hid_host.c" source)
hub_usb_extract("${source}" "bool hidh_xfer_cb(" "void hidh_close(" hid_xfer)
file(WRITE "${OUTPUT_DIR}/usb_hid_xfer.inc" "${hid_xfer}")
hub_generate_usb_usbh("${SDK_PATH}/lib/tinyusb/src/host/usbh.c" "${OUTPUT_DIR}/usbh.c")
file(READ "${OUTPUT_DIR}/usbh.c" source)
hub_usb_extract("${source}" "enum {\n  ENUM_RESET_DELAY_MS" "static bool enum_request_set_addr" enum_constants)
hub_usb_extract("${source}" "enum { HUB_ENUM_CLEANUP_TIMEOUT_MS" "// process device enumeration" cleanup)
hub_usb_extract("${source}" "static void process_enumeration(tuh_xfer_t* xfer) {"
    "  uint8_t const daddr = xfer->daddr;\n  uintptr_t const state" failure)
hub_usb_extract("${source}" "      case HCD_EVENT_DEVICE_ATTACH:\n"
    "        // due to the shared control buffer" attach_guard)
hub_usb_extract("${source}" "static bool enum_new_device(hcd_event_t* event) {"
    "static uint8_t get_new_address(" enum_start)
hub_usb_extract("${source}" "    case ENUM_ADDR0_DEVICE_DESC: {" "#if 0" descriptor_start)
file(WRITE "${OUTPUT_DIR}/usb_enum_recovery.inc"
    "${enum_constants}\n${cleanup}\n${failure}\n  ++successful_prefix_calls;\n"
    "  switch (xfer->user_data) {\n${descriptor_start}\n  default: break;\n  }\n}\n"
    "${enum_start}\n"
    "static void test_attach_dispatch_port(uint8_t hub, uint8_t port) {\n"
    "  hcd_event_t event = {.event_id = HCD_EVENT_DEVICE_ATTACH, .rhport = _dev0.rhport, .connection = {hub, port}};\n"
    "  bool in_isr = false;\n  switch (event.event_id) {\n${attach_guard}\n    ++allowed_attaches;\n    break;\n  }\n}\n"
    "static void test_attach_dispatch(void) { test_attach_dispatch_port(_dev0.hub_addr, _dev0.hub_port); }\n")

hub_generate_usb_hub("${SDK_PATH}/lib/tinyusb/src/host/hub.c" "${OUTPUT_DIR}/hub.c")
file(READ "${OUTPUT_DIR}/hub.c" source)
hub_usb_extract("${source}" "typedef struct\n{" "#if CFG_TUSB_DEBUG >= 2" hub_types)
hub_usb_extract("${source}" "bool hub_init(void)" "bool hub_open(" hub_init)
hub_usb_extract("${source}" "void hub_close(" "//--------------------------------------------------------------------+\n// Set Configure" hub_close)
hub_usb_extract("${source}" "bool hub_usb_hub_power_pending(void)" "static void config_set_port_power (" hub_power)
hub_usb_extract("${source}" "static void config_set_port_power (tuh_xfer_t* xfer)\n{"
    "//--------------------------------------------------------------------+\n// Connection Changes" hub_config)
string(APPEND source "\n/* test hub end */")
hub_usb_extract("${source}" "static void connection_clear_conn_change_complete (tuh_xfer_t* xfer)\n{"
    "/* test hub end */" hub_attach)
string(REPLACE "#endif" "" hub_attach "${hub_attach}")
file(WRITE "${OUTPUT_DIR}/usb_hub_timing.inc"
    "${hub_types}\n${hub_init}\n${hub_close}\n${hub_power}\n"
    "static void config_port_power_complete(tuh_xfer_t* xfer);\n${hub_config}\n"
    "static void connection_attach_complete(tuh_xfer_t* xfer);\n${hub_attach}\n")

file(READ "${SDK_PATH}/src/rp2350/hardware_regs/include/hardware/regs/usb.h" registers)
string(REGEX MATCHALL "#define USB_[A-Z0-9_]+_BITS[ \t]+_u\\(0x[0-9a-fA-F]+\\)" constants "${registers}")
string(JOIN "\n" constants ${constants})
string(REGEX MATCH "#define USB_LINESTATE_TUNING_RESET[ \t]+_u\\(0x[0-9a-fA-F]+\\)" tuning_reset "${registers}")
if(NOT tuning_reset)
    message(FATAL_ERROR "RP2350 USB timing reset constant missing")
endif()
file(WRITE "${OUTPUT_DIR}/usb_registers.h" "#define _u(value) value\n${constants}\n${tuning_reset}\n")

if(NOT CMAKE_SCRIPT_MODE_FILE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
endif()
