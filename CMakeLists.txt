cmake_minimum_required(VERSION 3.20)

set(PICO_BOARD pico2_w CACHE STRING "Target board")
if(NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/.deps/pico-sdk")
endif()
if(NOT EXISTS "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    message(FATAL_ERROR "Pico SDK is missing. Run tools/bootstrap.ps1 first.")
endif()
include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")

project(pico_bt_hub VERSION 0.3.6 LANGUAGES C CXX ASM)
option(HUB_CLASSIC_OPEN_TEST "Enable isolated, non-forwarding Classic Level 0 tests" OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

add_executable(pico_bt_hub
    src/main.c
    src/hub_output.c
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
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
    PICO_STDIO_USB_STDOUT_TIMEOUT_US=2000
)
pico_btstack_make_gatt_header(pico_bt_hub PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/hub.gatt")
pico_enable_stdio_usb(pico_bt_hub 1)
pico_enable_stdio_uart(pico_bt_hub 0)
pico_set_program_name(pico_bt_hub "Pico 2 W Bluetooth Hub")
if(HUB_CLASSIC_OPEN_TEST)
    pico_set_program_version(pico_bt_hub "${PROJECT_VERSION}-open-test")
else()
    pico_set_program_version(pico_bt_hub "${PROJECT_VERSION}")
endif()
pico_add_extra_outputs(pico_bt_hub)
