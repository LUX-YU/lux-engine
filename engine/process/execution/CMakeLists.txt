find_package(lux-cxx REQUIRED COMPONENTS compile_time)
find_package(stdexec CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_PROCESS_EXECUTION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_PROCESS_EXECUTION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_PROCESS_EXECUTION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/process/visibility.h
)

add_component(
    COMPONENT_NAME process_execution
    NAMESPACE      lux::engine::process
    OUTPUT_NAME    lux_engine_process_execution
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ExecutionRuntime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Timer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TimerFailureInjection.cpp
)

component_include_directories(
    process_execution
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_include_directories(
    process_execution
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
target_compile_definitions(process_execution PRIVATE LUX_PROCESS_EXECUTION_LIBRARY)
target_link_libraries(
    process_execution
    PUBLIC
        lux::engine::core::async_port
        STDEXEC::stdexec
)
component_add_transitive_commands(
    process_execution
    "find_package(lux-engine-core REQUIRED COMPONENTS async_port)"
    "find_package(stdexec CONFIG REQUIRED)"
)
lux_classify_target(
    TARGET  process_execution
    LAYER   PROCESS
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(process_timer_test test/timer_test.cpp)
target_include_directories(process_timer_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)
target_link_libraries(process_timer_test PRIVATE lux::engine::process::process_execution)
if(MSVC)
    target_compile_options(process_timer_test PRIVATE /UNDEBUG)
else()
    target_compile_options(process_timer_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  process_timer_test
    LAYER   PROCESS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME process_timer_test COMMAND process_timer_test)

add_executable(process_port_sender_test test/port_sender_test.cpp)
target_link_libraries(process_port_sender_test PRIVATE lux::engine::process::process_execution)
if(MSVC)
    target_compile_options(process_port_sender_test PRIVATE /UNDEBUG)
else()
    target_compile_options(process_port_sender_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  process_port_sender_test
    LAYER   PROCESS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME process_port_sender_test COMMAND process_port_sender_test)

add_executable(process_execution_runtime_test test/execution_runtime_test.cpp)
target_link_libraries(process_execution_runtime_test PRIVATE lux::engine::process::process_execution)
if(MSVC)
    target_compile_options(process_execution_runtime_test PRIVATE /UNDEBUG)
else()
    target_compile_options(process_execution_runtime_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  process_execution_runtime_test
    LAYER   PROCESS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME process_execution_runtime_test COMMAND process_execution_runtime_test)

add_executable(process_task_scope_test test/task_scope_test.cpp)
target_link_libraries(process_task_scope_test PRIVATE lux::engine::process::process_execution)
if(MSVC)
    target_compile_options(process_task_scope_test PRIVATE /UNDEBUG)
else()
    target_compile_options(process_task_scope_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  process_task_scope_test
    LAYER   PROCESS
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME process_task_scope_test COMMAND process_task_scope_test)
set_tests_properties(process_task_scope_test PROPERTIES TIMEOUT 30)

foreach(probe IN ITEMS task_scope_unconsumed_value_negative task_scope_unconsumed_error_negative)
    add_executable(${probe} EXCLUDE_FROM_ALL test/${probe}.cpp)
    target_link_libraries(${probe} PRIVATE lux::engine::process::process_execution)
    lux_classify_target(
        TARGET  ${probe}
        LAYER   PROCESS
        PRODUCT TEST
        ROLE    FOUNDATION
    )
    add_test(
        NAME ${probe}
        COMMAND
            ${CMAKE_COMMAND}
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DTARGET=${probe}
            -DEXPECTED_PATTERN=TaskScopeLifetimeSender
            -P ${CMAKE_SOURCE_DIR}/cmake/ExpectTargetCompileFailure.cmake
    )
endforeach()

add_executable(process_execution_benchmark test/process_execution_benchmark.cpp)
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_VARIABLE LUX_PROCESS_BENCHMARK_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(LUX_PROCESS_BENCHMARK_GIT_COMMIT STREQUAL "")
    set(LUX_PROCESS_BENCHMARK_GIT_COMMIT "unknown")
endif()
target_compile_definitions(
    process_execution_benchmark
    PRIVATE
        LUX_BENCHMARK_GIT_COMMIT="${LUX_PROCESS_BENCHMARK_GIT_COMMIT}"
        LUX_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)
target_link_libraries(process_execution_benchmark PRIVATE lux::engine::process::process_execution)
lux_classify_target(
    TARGET  process_execution_benchmark
    LAYER   PROCESS
    PRODUCT TEST
    ROLE    FOUNDATION
)

install_components(
    PROJECT_NAME lux-engine-process-execution
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::process
    COMPONENTS   process_execution
)
