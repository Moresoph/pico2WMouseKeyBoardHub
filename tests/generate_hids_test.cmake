cmake_minimum_required(VERSION 3.20)
if(NOT DEFINED BTSTACK_PATH OR NOT DEFINED OUTPUT_DIR)
    message(FATAL_ERROR "Pass BTSTACK_PATH and OUTPUT_DIR")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/HubBtstackHids.cmake")
hub_generate_patched_hids("${BTSTACK_PATH}/src/ble/gatt-service/hids_host.c" "${OUTPUT_DIR}/hids_host.c")
file(READ "${BTSTACK_PATH}/src/ble/gatt_client.c" source)
hub_hci_extract("${source}" "/*\n * Copyright" "#define BTSTACK_FILE__" license)
hub_hci_extract("${source}" "void gatt_client_deserialize_service("
    "void gatt_client_send_mtu_negotiation(" deserialize)
hub_hci_extract("${source}" "uint8_t gatt_client_att_status_to_error_code("
    "#ifdef ENABLE_GATT_CLIENT_CACHING\nvoid gatt_client_add_service_changed_handler" status)
file(WRITE "${OUTPUT_DIR}/gatt_values_under_test.inc" "${license}\n${deserialize}\n${status}")
