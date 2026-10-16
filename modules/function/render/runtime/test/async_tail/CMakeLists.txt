add_library(render_async_tail_plugin SHARED Provider.cpp)

target_link_libraries(render_async_tail_plugin PRIVATE render_features render_resource)

set_target_properties(render_async_tail_plugin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin"
)

add_executable(render_async_tail_test Main.cpp)

target_link_libraries(render_async_tail_test PRIVATE render_runtime lux::engine::process::process_execution)

add_dependencies(render_async_tail_test render_async_tail_plugin)

set_target_properties(render_async_tail_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/ipc/bin"
)

add_test(NAME render.runtime.async_dll_tail COMMAND render_async_tail_test $<TARGET_FILE:render_async_tail_plugin>)

set_tests_properties(render.runtime.async_dll_tail PROPERTIES TIMEOUT 90)
