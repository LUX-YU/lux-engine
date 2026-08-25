execute_process(
    COMMAND
        "${CMAKE_COMMAND}" --build "${BUILD_DIR}"
        --target ecs_missing_component_policy_probe
        -j 1
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(result EQUAL 0)
    message(FATAL_ERROR "missing ECS policies were accepted")
endif()
set(diagnostic "${output}\n${error}")
if(NOT diagnostic MATCHES "missing_component_policy.hpp: missing_component_policy: Component must declare exactly one LUX_COMPONENT_SNAPSHOT policy")
    message(FATAL_ERROR
        "ECS policy validation failed without the expected diagnostic:\n${diagnostic}"
    )
endif()
