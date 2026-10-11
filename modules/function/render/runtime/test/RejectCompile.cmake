execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "render_runtime_reject_${PROBE}" -j 4 -- -k 0
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors
)

file(WRITE "${BUILD_DIR}/f5_reject_${PROBE}.log" "${output}\n${errors}")

if(result EQUAL 0)
    message(FATAL_ERROR "Invalid Runtime contract compiled: ${PROBE}")
endif()

if(PROBE STREQUAL "ID")
    set(expected "C2440|conversion|convert")
else()
    set(expected "C2280|deleted")
endif()

if(NOT "${output}${errors}" MATCHES "${expected}")
    message(FATAL_ERROR "Unexpected compilation failure; see f5_reject_${PROBE}.log")
endif()
