execute_process(
    COMMAND "${TEST_EXECUTABLE}" --move-busy
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    TIMEOUT 30
)
if(NOT result STREQUAL "73" OR NOT error MATCHES "MOVE_BUSY terminate retained=1")
    message(FATAL_ERROR "Move assignment did not retain protected resources before terminate: ${result}\n${output}\n${error}")
endif()
message(STATUS "${error}")
