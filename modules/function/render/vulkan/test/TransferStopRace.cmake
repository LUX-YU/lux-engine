# Preserve the actual provider; instrument scheduling only in this test translation unit.
set(transfer_provider "${CMAKE_CURRENT_SOURCE_DIR}/src/resources/lifecycle/GpuTransferPipeline.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${transfer_provider}")
file(READ "${transfer_provider}" scheduled_transfer)
set(wait_boundary "            if (retireOneSubmittedSlot())")
set(close_boundary "        result_space_epoch_.notify_one();")
foreach(boundary IN ITEMS wait_boundary close_boundary)
    string(FIND "${scheduled_transfer}" "${${boundary}}" boundary_position)
    if(boundary_position LESS 0)
        message(FATAL_ERROR "Transfer stop-race scheduling boundary was removed: ${boundary}")
    endif()
endforeach()
string(REPLACE
    "${wait_boundary}"
    "            beforeTransferIdleWait();\n${wait_boundary}"
    scheduled_transfer "${scheduled_transfer}"
)
# Include the following blank line to distinguish shutdown from result-consumption notification.
string(REPLACE
    "${close_boundary}\n\n        std::vector<TransferCompletion> drained;"
    "${close_boundary}\n        afterTransferCloseWake();\n\n        std::vector<TransferCompletion> drained;"
    scheduled_transfer "${scheduled_transfer}"
)
string(FIND "${scheduled_transfer}" "afterTransferCloseWake();" close_hook)
if(close_hook LESS 0)
    message(FATAL_ERROR "Transfer shutdown notification boundary changed")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/GpuTransferPipelineScheduled.cpp" "${scheduled_transfer}")
add_executable(render_transfer_stop_race_test test/transfer_stop_race.cpp)
target_include_directories(
    render_transfer_stop_race_test
    PRIVATE sinclude pinclude "${CMAKE_CURRENT_BINARY_DIR}"
)
target_compile_definitions(render_transfer_stop_race_test PRIVATE LUX_FUNCTION_DLL_DISABLE)
target_link_libraries(
    render_transfer_stop_race_test
    PRIVATE render_vulkan GPUOpen::VulkanMemoryAllocator
)
if(MSVC)
    target_compile_options(render_transfer_stop_race_test PRIVATE /UNDEBUG /utf-8)
else()
    target_compile_options(render_transfer_stop_race_test PRIVATE -UNDEBUG)
endif()
if(LUX_RENDER_BUILD_GPU_TESTS)
    add_test(NAME render.transfer_stop_race COMMAND render_transfer_stop_race_test)
    set_tests_properties(render.transfer_stop_race PROPERTIES TIMEOUT 10 LABELS gpu)
endif()
