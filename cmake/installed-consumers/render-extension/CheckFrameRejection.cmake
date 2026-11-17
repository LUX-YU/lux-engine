execute_process(COMMAND "${PROBE}" "${CASE}" RESULT_VARIABLE result)
if(NOT result EQUAL 86)
    message(FATAL_ERROR "Expected ownership termination (86), received ${result} for ${CASE}")
endif()
