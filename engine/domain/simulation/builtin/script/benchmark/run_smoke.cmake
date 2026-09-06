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
    micro-event-wait
    scene-event-idle
    scene-event-fanout
    scene-event-sparse
    scene-update-heavy
    scene-cpp-update-heavy
    scene-cpp-sequence
    scene-region-numeric
    graph-build
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

foreach(requirements IN ITEMS 1 4 16 64)
    execute_process(COMMAND "${EXECUTABLE}" --group micro-event-wait --mode diagnostic
        --size 64 --frames 2 --warmups 1 --event-route targeted --event-requirements ${requirements}
        --output "${OUTPUT_DIR}/targeted-${requirements}.csv"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 60)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "targeted Event ${requirements} requirements failed: ${output}\n${error}")
    endif()
endforeach()
