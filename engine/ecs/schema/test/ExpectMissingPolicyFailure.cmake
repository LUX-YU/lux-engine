execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        "-DLUX_ECS_COMPONENT_HEADER=${HEADER}"
        -P "${VALIDATOR}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(result EQUAL 0)
    message(FATAL_ERROR "missing ECS policies were accepted")
endif()
set(diagnostic "${output}\n${error}")
if(NOT diagnostic MATCHES "must each provide")
    message(FATAL_ERROR
        "ECS policy validation failed without the expected diagnostic:\n${diagnostic}"
    )
endif()
