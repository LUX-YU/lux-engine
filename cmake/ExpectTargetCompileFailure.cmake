if(NOT DEFINED BUILD_DIR OR NOT DEFINED TARGET)
    message(FATAL_ERROR "BUILD_DIR and TARGET are required")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target "${TARGET}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)

if(result EQUAL 0)
    message(FATAL_ERROR "Expected target '${TARGET}' to fail compilation")
endif()

if(DEFINED EXPECTED_PATTERN)
    set(combined_output "${output}\n${error}")
    if(NOT combined_output MATCHES "${EXPECTED_PATTERN}")
        message(
            FATAL_ERROR
            "Target '${TARGET}' failed, but not for the expected reason "
            "('${EXPECTED_PATTERN}').\n${combined_output}"
        )
    endif()
endif()

message(STATUS "Target '${TARGET}' failed compilation for the expected reason")
