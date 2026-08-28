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

install_components(
    PROJECT_NAME lux-engine-process-execution
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::process
    COMPONENTS   process_execution
)
