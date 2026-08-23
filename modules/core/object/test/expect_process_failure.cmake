if(NOT DEFINED PROBE)
    message(FATAL_ERROR "PROBE is required")
endif()

execute_process(COMMAND "${PROBE}" RESULT_VARIABLE result)
if(result EQUAL 0)
    message(FATAL_ERROR "Expected '${PROBE}' to reject the contract violation")
endif()
