execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "render_vulkan_reject_${PROBE}" -j 4 -- -k 0
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
)

file(WRITE "${BUILD_DIR}/vulkan_reject_${PROBE}.log" "${output}\n${errors}")

string(FIND "${output}${errors}" "${HEADER}" header_position)

if(result EQUAL 0 OR header_position EQUAL -1 OR NOT "${output}${errors}" MATCHES "C1083|No such file|file not found")
    message(FATAL_ERROR "Forbidden Foundation dependency probe failed unexpectedly: ${PROBE}")
endif()
