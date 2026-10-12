execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${TARGET}" -j 4 -- -k 0
    RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE errors
)
file(WRITE "${BUILD_DIR}/${TARGET}.log" "${output}\n${errors}")
if(code EQUAL 0)
    message(FATAL_ERROR "Expected rejection: ${TARGET}")
endif()
if(NOT "${output}${errors}" MATCHES "${EXPECTED}")
    message(FATAL_ERROR "Unrelated rejection: ${output}${errors}")
endif()
