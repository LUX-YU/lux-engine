find_package(lux-cxx REQUIRED COMPONENTS compile_time container)
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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ExecutionError.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ExecutionRuntime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Task.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Timer.cpp
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
        lux::engine::core::error
        lux::cxx::compile_time
        lux::cxx::container
        STDEXEC::stdexec
)

component_add_transitive_commands(
    process_execution
    "find_package(lux-engine-core REQUIRED COMPONENTS async_port error)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time container)"
    "find_package(stdexec CONFIG REQUIRED)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-process-execution
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::process
    COMPONENTS   process_execution
)

if(BUILD_TESTING)
    add_executable(execution_error_test test/ExecutionErrorTest.cpp)
    target_link_libraries(execution_error_test PRIVATE lux::engine::process::process_execution)
    add_test(NAME process.execution_errors COMMAND execution_error_test)
    add_test(NAME process.execution_errors_collision COMMAND execution_error_test --collision)

    add_executable(task_scope_test test/TaskScopeTest.cpp)
    target_link_libraries(task_scope_test PRIVATE lux::engine::process::process_execution)
    if(MSVC)
        target_compile_options(task_scope_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(task_scope_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME process.task_scope COMMAND task_scope_test)
    set_tests_properties(process.task_scope PROPERTIES TIMEOUT 15)
endif()
