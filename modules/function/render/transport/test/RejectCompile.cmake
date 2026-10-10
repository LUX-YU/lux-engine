execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${TARGET_NAME}" -j 4 -- -k 0
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors
)

file(WRITE "${BUILD_DIR}/${TARGET_NAME}.log" "${output}\n${errors}")

if(result EQUAL 0)
    message(FATAL_ERROR "Forbidden ${TARGET_NAME} consumer compiled")
endif()

if(NOT "${output}${errors}" MATCHES "${EXPECTED}")
    message(FATAL_ERROR "Unexpected failure; see ${TARGET_NAME}.log")
endif()
