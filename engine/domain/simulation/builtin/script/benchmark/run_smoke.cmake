cmake_minimum_required(VERSION 3.22)

if(NOT DEFINED EXECUTABLE OR NOT EXISTS "${EXECUTABLE}")
    message(FATAL_ERROR "script runtime benchmark executable is required")
endif()
if(NOT DEFINED OUTPUT_DIR OR OUTPUT_DIR STREQUAL "")
    message(FATAL_ERROR "script runtime benchmark output directory is required")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
foreach(group IN ITEMS
    micro-sync
    micro-hook-channel
    micro-async
    micro-lifecycle
    scene-update-heavy
    scene-gameplay-mixed
    scene-suspended-idle
    scene-resume-storm
    scene-object-churn
    scheduler-next-step
    scheduler-simulation-delay
    integration-real-delay
)
    execute_process(
        COMMAND
            "${EXECUTABLE}"
            --group "${group}"
            --mode diagnostic
            --size 64
            --frames 4
            --ready 32
            --resume-budget 16
            --output "${OUTPUT_DIR}/${group}.csv"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 60
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${group} failed (${result}):\n${output}\n${error}")
    endif()
endforeach()
