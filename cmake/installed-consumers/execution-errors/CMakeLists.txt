cmake_minimum_required(VERSION 3.28)

project(ExecutionDiagnosticConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)
find_package(lux-engine-core REQUIRED COMPONENTS semantic)

enable_testing()

add_executable(
    execution_errors
    "${CMAKE_CURRENT_LIST_DIR}/../../../engine/process/execution/test/ExecutionErrorTest.cpp"
)
target_link_libraries(execution_errors PRIVATE lux::engine::process::process_execution)
add_test(NAME installed.execution_errors COMMAND execution_errors)
add_test(NAME installed.execution_errors_collision COMMAND execution_errors --collision)

add_executable(semantic_identity "${CMAKE_CURRENT_LIST_DIR}/../../../modules/core/semantic/test/identity.cpp")
target_link_libraries(semantic_identity PRIVATE lux::engine::core::semantic)
add_test(NAME installed.semantic_identity COMMAND semantic_identity)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/header.cpp" "#include <lux/engine/process/ExecutionError.hpp>\n")
add_library(execution_error_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/header.cpp")
target_link_libraries(execution_error_header PRIVATE lux::engine::process::process_execution)

if(MSVC)
    foreach(target execution_errors semantic_identity execution_error_header)
        target_compile_options(${target} PRIVATE /permissive- /GR- /we4541 /utf-8)
    endforeach()
else()
    foreach(target execution_errors semantic_identity execution_error_header)
        target_compile_options(${target} PRIVATE -fno-rtti)
    endforeach()
endif()
