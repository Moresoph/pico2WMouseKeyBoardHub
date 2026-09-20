cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED BTSTACK_PATH OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "Pass -DBTSTACK_PATH=<SDK>/lib/btstack -DOUTPUT_DIR=<build>/tests/generated")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/HubBtstackReconnect.cmake")
hub_generate_patched_hci("${BTSTACK_PATH}/src/hci.c" "${OUTPUT_DIR}/hci.c")
file(READ "${OUTPUT_DIR}/hci.c" source)

# Compile the actual patched handler and original GAP APIs in a small native
# harness. Only controller scheduling, allocation and event delivery are doubles.
hub_hci_extract("${source}" "/*\n * Copyright" "#define BTSTACK_FILE__" license)
hub_hci_extract("${source}"
    "bool hci_is_le_connection_type(bd_addr_type_t address_type){"
    "/**\n * count connections" address_types)
hub_hci_extract("${source}"
    "hci_connection_t * hci_connection_for_bd_addr_and_type(const bd_addr_t  addr, bd_addr_type_t addr_type){"
    "inline static void hci_connection_clear_authentication_flags(" lookup)
hub_hci_extract("${source}"
    "static void hci_create_gap_connection_complete_event(const uint8_t * hci_event, uint8_t * gap_event) {"
    "static void hci_handle_le_meta_event(uint8_t * packet, uint16_t size){" complete)
hub_hci_extract("${source}"
    "uint8_t gap_connect(const bd_addr_t addr, bd_addr_type_t addr_type) {"
    "/**\n * @brief Set connection parameters for outgoing connections" connect)
file(WRITE "${OUTPUT_DIR}/hci_reconnect_under_test.inc" "${license}\n${address_types}\n${lookup}\n${complete}\n${connect}")
