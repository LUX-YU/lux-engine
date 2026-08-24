if(NOT DEFINED PROGRAM OR PROGRAM STREQUAL "")
    message(FATAL_ERROR "PROGRAM is required")
endif()

execute_process(
    COMMAND "${PROGRAM}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)

if(result EQUAL 0)
    message(FATAL_ERROR
        "Expected process failure, but the probe exited successfully.\n"
        "stdout:\n${output}\n"
        "stderr:\n${error}"
    )
endif()
