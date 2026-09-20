execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${OUTPUT_DIR}"
    -G "${GENERATOR}" "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}"
    "-DPICO_BTSTACK_PATH=${BTSTACK_PATH}" "-DHUB_OVERLAY_TEST_CASE=${CASE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0 OR "${output}${error}" MATCHES "FAIL_OPEN")
    message(FATAL_ERROR "Overlay ${CASE} guard unexpectedly accepted invalid input:\n${output}\n${error}")
endif()
if(NOT "${output}${error}" MATCHES "requires Pico SDK|differs from pinned|Expected exactly one|anchor missing|anchor is ambiguous")
    message(FATAL_ERROR "Overlay ${CASE} failed for an unrelated reason:\n${output}\n${error}")
endif()
