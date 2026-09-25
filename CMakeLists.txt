cmake_minimum_required(VERSION 3.20)

set(PICO_BOARD pico2_w CACHE STRING "Target board")
if(NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/.deps/pico-sdk")
endif()
if(NOT EXISTS "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    message(FATAL_ERROR "Pico SDK is missing. Run tools/bootstrap.ps1 first.")
endif()
include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")

project(pico_bt_hub VERSION 0.4.3 LANGUAGES C CXX ASM)
option(HUB_CLASSIC_OPEN_TEST "Enable Classic Level 0 inputs with explicit forwarding authorization" OFF)
option(HUB_CLASSIC_OUTPUT_TEST "Experimental single-PC Classic HID output instead of BLE" OFF)
option(HUB_CLASSIC_MOUSE_7P5_TEST "Request mouse-only 7.5ms interval in Classic normal mode" OFF)
option(HUB_CLASSIC_OUTPUT_7P5_TEST "Use 7.5ms ordinary Classic output pacing instead of 2.5ms" OFF)
if(HUB_CLASSIC_MOUSE_7P5_TEST AND NOT HUB_CLASSIC_OUTPUT_TEST)
    message(FATAL_ERROR "HUB_CLASSIC_MOUSE_7P5_TEST requires HUB_CLASSIC_OUTPUT_TEST")
endif()
if(HUB_CLASSIC_OUTPUT_7P5_TEST AND NOT HUB_CLASSIC_OUTPUT_TEST)
    message(FATAL_ERROR "HUB_CLASSIC_OUTPUT_7P5_TEST requires HUB_CLASSIC_OUTPUT_TEST")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()
include("${CMAKE_CURRENT_LIST_DIR}/cmake/HubBtstackReconnect.cmake")
hub_patch_btstack_reconnect()
if(HUB_CLASSIC_OUTPUT_TEST)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/HubBtstackClassicMux.cmake")
    hub_patch_btstack_classic_mux()
    set(HUB_OUTPUT_SOURCE src/hub_classic_output.c src/hub_classic_mux.c)
    set(HUB_GATT_SOURCE src/classic/hub.gatt)
else()
    set(HUB_OUTPUT_SOURCE src/hub_output.c)
    set(HUB_GATT_SOURCE src/hub.gatt)
endif()

add_executable(pico_bt_hub
    src/main.c
    ${HUB_OUTPUT_SOURCE}
    src/hub_output_probe.c
    src/hub_output_timer.c
    src/hub_output_store.c
    src/hub_input.c
    src/hub_router.c
    src/hub_passkey.c
    src/hub_button.c
    src/hub_bootsel.c
    src/hub_smp_decode.c
    src/hub_smp_trace.c
    src/hub_security_policy.c
    src/hub_classic_decode.c
    src/hub_diag_ring.c
    src/hub_clock.c
    src/hub_latency.c
    src/hub_perf.c
)
target_include_directories(pico_bt_hub PRIVATE include)
target_link_libraries(pico_bt_hub PRIVATE
    pico_stdlib
    pico_cyw43_arch_none
    pico_btstack_cyw43
    pico_btstack_ble
    pico_btstack_classic
)
target_compile_options(pico_bt_hub PRIVATE -Wall -Wextra)
target_compile_definitions(pico_bt_hub PRIVATE
    HUB_CLASSIC_OPEN_TEST=$<BOOL:${HUB_CLASSIC_OPEN_TEST}>
    HUB_CLASSIC_OUTPUT_TEST=$<BOOL:${HUB_CLASSIC_OUTPUT_TEST}>
    HUB_CLASSIC_MOUSE_7P5_TEST=$<BOOL:${HUB_CLASSIC_MOUSE_7P5_TEST}>
    HUB_CLASSIC_OUTPUT_7P5_TEST=$<BOOL:${HUB_CLASSIC_OUTPUT_7P5_TEST}>
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
    PICO_STDIO_USB_STDOUT_TIMEOUT_US=2000
)
pico_btstack_make_gatt_header(pico_bt_hub PRIVATE "${CMAKE_CURRENT_LIST_DIR}/${HUB_GATT_SOURCE}")
pico_enable_stdio_usb(pico_bt_hub 1)
pico_enable_stdio_uart(pico_bt_hub 0)
pico_set_program_name(pico_bt_hub "Pico 2 W Bluetooth Hub")
if(HUB_CLASSIC_MOUSE_7P5_TEST AND HUB_CLASSIC_OUTPUT_7P5_TEST)
    pico_set_program_version(pico_bt_hub "${PROJECT_VERSION}-classic-output-test-mouse7p5-output7p5")
elseif(HUB_CLASSIC_OUTPUT_7P5_TEST)
    pico_set_program_version(pico_bt_hub "${PROJECT_VERSION}-classic-output-test-output7p5")
elseif(HUB_CLASSIC_MOUSE_7P5_TEST)
    pico_set_program_version(pico_bt_hub "${PROJECT_VERSION}-classic-output-test-mouse7p5")
elseif(HUB_CLASSIC_OUTPUT_TEST)
    pico_set_program_version(pico_bt_hub "${PROJECT_VERSION}-classic-output-test-acl8")
elseif(HUB_CLASSIC_OPEN_TEST)
    pico_set_program_version(pico_bt_hub "${PROJECT_VERSION}-open-test-mouse-buffer")
else()
    pico_set_program_version(pico_bt_hub "${PROJECT_VERSION}-mouse-buffer")
endif()
pico_add_extra_outputs(pico_bt_hub)
