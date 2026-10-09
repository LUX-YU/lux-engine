cmake_minimum_required(VERSION 3.28)

project(LogOutputConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)

set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(lux-engine-core REQUIRED COMPONENTS log)

enable_testing()

add_executable(log_output "${CMAKE_CURRENT_LIST_DIR}/../../../modules/core/log/test/output_lifetime.cpp")

target_link_libraries(log_output PRIVATE lux::engine::core::log)

add_test(NAME installed.log_output COMMAND log_output)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/header.cpp" "#include <lux/engine/log/Log.hpp>\n")

add_library(log_output_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/header.cpp")

target_link_libraries(log_output_header PRIVATE lux::engine::core::log)

if(MSVC)
    target_compile_options(log_output PRIVATE /permissive- /GR- /we4541 /utf-8)

    target_compile_options(log_output_header PRIVATE /permissive- /GR- /we4541 /utf-8)
else()
    target_compile_options(log_output PRIVATE -fno-rtti)

    target_compile_options(log_output_header PRIVATE -fno-rtti)
endif()
