file(READ "${ROOT}/src/main.c" source)

function(extract start end output)
    string(FIND "${source}" "${start}" begin)
    string(FIND "${source}" "${end}" finish)
    if(begin LESS 0 OR finish LESS begin)
        message(FATAL_ERROR "Production PC pairing test anchors missing: ${start}")
    endif()
    math(EXPR size "${finish}-${begin}")
    string(SUBSTRING "${source}" ${begin} ${size} body)
    set(${output} "${body}" PARENT_SCOPE)
endfunction()

extract("static bool ble_pairing_active(" "static void set_pairing_active(" active)
extract("static void set_pairing_active(" "static void refresh_pairing_mode(" set_active)
extract("static void refresh_pairing_mode(" "static void finish_keyboard_capture(" refresh)
extract("bool hub_security_pairing_open(" "bool hub_security_input_pairing_blocked(" admission)
extract("static bool start_pc_pairing(" "static void poll_enrollment_button(" start)
extract("static void poll_enrollment_button(" "static uint8_t classic_peer_io(" button)
extract("    if (strcmp(args[0], \"pair\") == 0 && argc == 2) {"
    "    if (argc == 3 && (strcmp(args[0], \"confirm\")" command)
extract("        case SM_EVENT_JUST_WORKS_REQUEST:"
    "        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:" just_works)
extract("        case SM_EVENT_PAIRING_COMPLETE:"
    "        case SM_EVENT_REENCRYPTION_COMPLETE:" complete)
file(WRITE "${OUTPUT}"
    "${active}\n${set_active}\n${refresh}\n${admission}\n${start}\n"
    "#if HUB_DUAL_BLUETOOTH\n${button}\n#endif\n"
    "static void pair_command(const char *argument) {\n"
    "    const char *args[] = {\"pair\", argument};\n"
    "    unsigned argc = 2;\n${command}\n}\n"
    "static void pairing_event(uint8_t event, uint16_t value, uint8_t status) {\n"
    "    uint8_t packet[] = {(uint8_t)value, (uint8_t)(value >> 8), status};\n"
    "    uint16_t handle;\n    switch (event) {\n${just_works}\n${complete}\n"
    "        default: assert(false);\n    }\n}\n")
