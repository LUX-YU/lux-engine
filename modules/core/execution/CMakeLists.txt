find_package(lux-cxx REQUIRED COMPONENTS compile_time container)
find_package(stdexec CONFIG REQUIRED GLOBAL)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_CORE_EXECUTION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_CORE_EXECUTION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_CORE_EXECUTION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/core/execution/visibility.h
)

add_component(
    COMPONENT_NAME core_execution
    NAMESPACE      lux::engine::core
    OUTPUT_NAME    lux_engine_core_execution
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ExecutionError.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ExecutionRuntime.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RunLoop.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Task.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Timer.cpp
)

component_include_directories(
    core_execution
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_include_directories(
    core_execution
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
target_compile_definitions(core_execution PRIVATE LUX_CORE_EXECUTION_LIBRARY)
target_link_libraries(
    core_execution
    PUBLIC
        lux::engine::core::async_port
        lux::engine::core::error
        lux::cxx::compile_time
        lux::cxx::container
        STDEXEC::stdexec
)

component_add_transitive_commands(
    core_execution
    "find_package(lux-engine-core REQUIRED COMPONENTS async_port error)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time container)"
    "find_package(stdexec CONFIG REQUIRED GLOBAL)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-core-execution
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::core
    COMPONENTS   core_execution
)

if(BUILD_TESTING)
    foreach(header
        CompletionWork ExecutionError ExecutionRuntime PortSender RunLoop
        Scheduler Task TaskInfo TaskScope Timer
    )
        file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cpp" CONTENT
            "#include <lux/engine/core/execution/${header}.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
        )
        add_library(execution_header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/headers/${header}.cpp")
        target_link_libraries(execution_header_${header} PRIVATE core_execution)
    endforeach()

    add_executable(execution_run_loop_test test/RunLoopTest.cpp)
    target_link_libraries(execution_run_loop_test PRIVATE core_execution)
    add_test(NAME core.execution_run_loop COMMAND execution_run_loop_test)
    set_tests_properties(core.execution_run_loop PROPERTIES TIMEOUT 15)

    get_filename_component(execution_source_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
    add_test(NAME core.execution_standalone
        COMMAND ${CMAKE_COMMAND}
            "-DSOURCE_DIR=${execution_source_root}"
            "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
            "-DTOOLSET_DIR=${lux-cmake-toolset_DIR}"
            "-DDEPENDENCY_DIR=${lux-cxx_DIR}"
            "-DSTDEXEC_DIR=${stdexec_DIR}"
            "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            "-DCXX_FLAGS=${CMAKE_CXX_FLAGS}"
            "-DCONFIG=${CMAKE_BUILD_TYPE}"
            "-DGENERATOR=${CMAKE_GENERATOR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/test/Standalone.cmake"
    )
    set_tests_properties(core.execution_standalone PROPERTIES TIMEOUT 600)

    add_executable(execution_error_test test/ExecutionErrorTest.cpp)
    target_link_libraries(execution_error_test PRIVATE lux::engine::core::core_execution)
    add_test(NAME process.execution_errors COMMAND execution_error_test)
    add_test(NAME process.execution_errors_collision COMMAND execution_error_test --collision)

    add_executable(task_scope_test test/TaskScopeTest.cpp)
    target_link_libraries(task_scope_test PRIVATE lux::engine::core::core_execution)
    if(MSVC)
        target_compile_options(task_scope_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(task_scope_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME process.task_scope COMMAND task_scope_test)
    set_tests_properties(process.task_scope PROPERTIES TIMEOUT 15)

    add_executable(port_sender_test test/PortSenderTest.cpp)
    target_link_libraries(port_sender_test PRIVATE lux::engine::core::core_execution)
    add_test(NAME process.port_sender COMMAND port_sender_test)
    set_tests_properties(process.port_sender PROPERTIES TIMEOUT 15)
endif()
