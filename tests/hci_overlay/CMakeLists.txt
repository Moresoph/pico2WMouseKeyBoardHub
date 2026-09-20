cmake_minimum_required(VERSION 3.20)
project(hci_overlay_tests NONE)
enable_testing()

if(NOT DEFINED PICO_BTSTACK_PATH)
    get_filename_component(PICO_BTSTACK_PATH
        "${CMAKE_CURRENT_LIST_DIR}/../../.deps/pico-sdk/lib/btstack" ABSOLUTE)
endif()
set(PICO_SDK_VERSION_STRING "2.3.1")
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/HubBtstackReconnect.cmake")

if(HUB_OVERLAY_TEST_CASE STREQUAL "sdk")
    set(PICO_SDK_VERSION_STRING "2.3.2")
elseif(HUB_OVERLAY_TEST_CASE STREQUAL "hash")
    file(READ "${PICO_BTSTACK_PATH}/src/hci.c" changed_source)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/changed_hci.c" "${changed_source}\n/* unexpected change */\n")
    hub_generate_patched_hci("${CMAKE_CURRENT_BINARY_DIR}/changed_hci.c"
        "${CMAKE_CURRENT_BINARY_DIR}/unexpected.c")
    message(FATAL_ERROR "FAIL_OPEN: changed SDK accepted")
elseif(HUB_OVERLAY_TEST_CASE STREQUAL "anchor_missing")
    hub_hci_find_once("one" "two" unused)
elseif(HUB_OVERLAY_TEST_CASE STREQUAL "anchor_duplicate")
    hub_hci_find_once("one one" "one" unused)
endif()

add_library(pico_btstack_base INTERFACE)
target_sources(pico_btstack_base INTERFACE
    "${PICO_BTSTACK_PATH}/src/hci_cmd.c" "${PICO_BTSTACK_PATH}/src/hci.c")
if(HUB_OVERLAY_TEST_CASE STREQUAL "missing")
    set_property(TARGET pico_btstack_base PROPERTY INTERFACE_SOURCES "${PICO_BTSTACK_PATH}/src/hci_cmd.c")
elseif(HUB_OVERLAY_TEST_CASE STREQUAL "duplicate")
    target_sources(pico_btstack_base INTERFACE "${PICO_BTSTACK_PATH}/src/hci.c")
endif()
hub_patch_btstack_reconnect()
get_target_property(actual pico_btstack_base INTERFACE_SOURCES)
set(expected "${PICO_BTSTACK_PATH}/src/hci_cmd.c;${CMAKE_CURRENT_BINARY_DIR}/generated/btstack/hci.c")
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "Overlay did not preserve and replace target sources exactly: ${actual}")
endif()

if(NOT DEFINED HUB_OVERLAY_TEST_CASE)
    foreach(case IN ITEMS sdk hash missing duplicate anchor_missing anchor_duplicate)
        add_test(NAME hci_overlay_reject_${case}
            COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}"
                "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/${case}"
                "-DBTSTACK_PATH=${PICO_BTSTACK_PATH}" "-DCASE=${case}"
                "-DGENERATOR=${CMAKE_GENERATOR}" "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
                -P "${CMAKE_CURRENT_LIST_DIR}/reject.cmake")
    endforeach()
endif()
