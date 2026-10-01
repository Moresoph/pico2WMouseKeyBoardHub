get_filename_component(HUB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(BTSTACK "${HUB_ROOT}/.deps/pico-sdk/lib/btstack/src")
idf_component_register(
    SRCS "main.c" "${HUB_ROOT}/src/hub_link.c" "${HUB_ROOT}/src/hub_remote_hid.c"
         "${BTSTACK}/btstack_hid_parser.c" "${BTSTACK}/btstack_util.c"
    INCLUDE_DIRS "." "${HUB_ROOT}/include" "${BTSTACK}"
    REQUIRES bt esp_hid nvs_flash esp_driver_uart esp_driver_gpio esp_timer console)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
