cmake_minimum_required(VERSION 3.20)

set(PICO_BOARD pico2_w CACHE STRING "Target board")
if(NOT DEFINED PICO_SDK_PATH)
    set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/.deps/pico-sdk")
endif()
if(NOT EXISTS "${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
    message(FATAL_ERROR "Pico SDK is missing. Run tools/bootstrap.ps1 first.")
endif()
include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")

project(pico_bt_hub VERSION 0.4.21 LANGUAGES C CXX ASM)
option(HUB_DUAL_BLUETOOTH "ESP32 UART inputs; onboard Bluetooth reserved for BLE computer outputs" OFF)
option(HUB_CLASSIC_OPEN_TEST "Enable Classic Level 0 inputs with explicit forwarding authorization" OFF)
option(HUB_CLASSIC_OUTPUT_TEST "Experimental single-PC Classic HID output instead of BLE" OFF)
option(HUB_CLASSIC_INPUT_CENTRAL "Prefer Pico Central on Classic HID input links" ON)
option(HUB_CLASSIC_MOUSE_7P5_TEST "Request mouse-only 7.5ms interval in Classic normal mode" OFF)
option(HUB_CLASSIC_OUTPUT_7P5_TEST "Use 7.5ms ordinary Classic output pacing instead of 2.5ms" OFF)
option(HUB_USB_HOST_PROBE "Experimental native USB Host enumeration with GPIO UART console" OFF)
if(HUB_DUAL_BLUETOOTH AND (HUB_USB_HOST_PROBE OR HUB_CLASSIC_OUTPUT_TEST OR HUB_CLASSIC_OPEN_TEST))
    message(FATAL_ERROR "Dual Bluetooth uses USB CDC + BLE output; do not combine experimental backends")
endif()
if(HUB_USB_HOST_PROBE AND HUB_CLASSIC_OUTPUT_TEST)
    message(FATAL_ERROR "The USB Host probe requires the BLE output backend")
endif()
if(HUB_USB_HOST_PROBE)
    set(LOG 2) # Capture enumeration details in RAM, never print from USB interrupts.
endif()
if(HUB_CLASSIC_MOUSE_7P5_TEST AND NOT HUB_CLASSIC_OUTPUT_TEST)
    message(FATAL_ERROR "HUB_CLASSIC_MOUSE_7P5_TEST requires HUB_CLASSIC_OUTPUT_TEST")
endif()
if(HUB_CLASSIC_OUTPUT_7P5_TEST AND NOT HUB_CLASSIC_OUTPUT_TEST)
    message(FATAL_ERROR "HUB_CLASSIC_OUTPUT_7P5_TEST requires HUB_CLASSIC_OUTPUT_TEST")
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()
if(HUB_USB_HOST_PROBE)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/HubTinyUsbHost.cmake")
    hub_patch_tinyusb_host()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/cmake/HubBtstackReconnect.cmake")
hub_patch_btstack_reconnect()
include("${CMAKE_CURRENT_LIST_DIR}/cmake/HubBtstackHids.cmake")
hub_patch_btstack_hids()
include("${CMAKE_CURRENT_LIST_DIR}/cmake/HubBtstackClassicRecovery.cmake")
hub_patch_btstack_classic_recovery()
if(HUB_CLASSIC_OUTPUT_TEST)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/HubBtstackClassicMux.cmake")
    hub_patch_btstack_classic_mux()
    set(HUB_OUTPUT_SOURCE src/hub_classic_output.c src/hub_classic_mux.c)
    set(HUB_GATT_SOURCE src/classic/hub.gatt)
else()
    set(HUB_OUTPUT_SOURCE src/hub_output.c)
    set(HUB_GATT_SOURCE src/hub.gatt)
endif()

if(HUB_DUAL_BLUETOOTH)
    set(HUB_INPUT_SOURCE src/hub_uart_input.c src/hub_external_input.c src/hub_link.c)
else()
    set(HUB_INPUT_SOURCE src/hub_input.c)
endif()
add_executable(pico_bt_hub
    src/main.c
    ${HUB_OUTPUT_SOURCE}
    src/hub_output_probe.c
    src/hub_output_timer.c
    src/hub_output_store.c
    ${HUB_INPUT_SOURCE}
    src/hub_hogp.c
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
if(HUB_DUAL_BLUETOOTH)
    target_link_libraries(pico_bt_hub PRIVATE hardware_uart pico_rand)
endif()
if(HUB_USB_HOST_PROBE)
    target_sources(pico_bt_hub PRIVATE src/hub_usb_host.c src/hub_usb_descriptor.c src/hub_usb_trace.c
        src/hub_usb_mouse.c src/hub_usb_mouse_decode.c src/hub_usb_wire.c)
    target_include_directories(pico_bt_hub PRIVATE include/usb_host)
    target_link_libraries(pico_bt_hub PRIVATE tinyusb_host hardware_dma hardware_clocks)
endif()
target_link_libraries(pico_bt_hub PRIVATE
    pico_stdlib
    pico_cyw43_arch_none
    pico_btstack_cyw43
    pico_btstack_ble
    pico_btstack_classic
)
target_compile_options(pico_bt_hub PRIVATE -Wall -Wextra)
target_compile_definitions(pico_bt_hub PRIVATE
    HUB_DUAL_BLUETOOTH=$<BOOL:${HUB_DUAL_BLUETOOTH}>
    HUB_CLASSIC_OPEN_TEST=$<BOOL:${HUB_CLASSIC_OPEN_TEST}>
    HUB_CLASSIC_OUTPUT_TEST=$<BOOL:${HUB_CLASSIC_OUTPUT_TEST}>
    HUB_CLASSIC_INPUT_CENTRAL=$<BOOL:${HUB_CLASSIC_INPUT_CENTRAL}>
    HUB_CLASSIC_MOUSE_7P5_TEST=$<BOOL:${HUB_CLASSIC_MOUSE_7P5_TEST}>
    HUB_CLASSIC_OUTPUT_7P5_TEST=$<BOOL:${HUB_CLASSIC_OUTPUT_7P5_TEST}>
    HUB_USB_HOST_PROBE=$<BOOL:${HUB_USB_HOST_PROBE}>
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
    PICO_STDIO_USB_STDOUT_TIMEOUT_US=2000
)
pico_btstack_make_gatt_header(pico_bt_hub PRIVATE "${CMAKE_CURRENT_LIST_DIR}/${HUB_GATT_SOURCE}")
if(HUB_USB_HOST_PROBE)
    pico_enable_stdio_usb(pico_bt_hub 0)
    pico_enable_stdio_uart(pico_bt_hub 1)
    target_compile_definitions(pico_bt_hub PRIVATE
        PICO_DEFAULT_UART=0 PICO_DEFAULT_UART_TX_PIN=0 PICO_DEFAULT_UART_RX_PIN=1
        PICO_DEFAULT_UART_BAUD_RATE=115200)
else()
    pico_enable_stdio_usb(pico_bt_hub 1)
    pico_enable_stdio_uart(pico_bt_hub 0)
endif()
pico_set_program_name(pico_bt_hub "Pico 2 W Bluetooth Hub")
if(HUB_DUAL_BLUETOOTH)
    pico_set_program_version(pico_bt_hub "0.6.9-esc-switch-fix")
elseif(HUB_USB_HOST_PROBE)
    if(HUB_CLASSIC_OPEN_TEST)
        pico_set_program_version(pico_bt_hub "${PROJECT_VERSION}-open-test-usb-host-probe")
    else()
        pico_set_program_version(pico_bt_hub "${PROJECT_VERSION}-usb-host-probe")
    endif()
elseif(HUB_CLASSIC_MOUSE_7P5_TEST AND HUB_CLASSIC_OUTPUT_7P5_TEST)
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
