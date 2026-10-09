cmake_minimum_required(VERSION 3.28)

project(ContextExtensionsConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(lux-engine-context REQUIRED COMPONENTS engine_context)

enable_testing()

add_executable(context_extensions "${CMAKE_CURRENT_LIST_DIR}/../../../engine/context/test/extensions.cpp")

target_link_libraries(context_extensions PRIVATE lux::engine::engine_context)

add_test(NAME installed.context_extensions COMMAND context_extensions)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/header.cpp" "#include <lux/engine/ContextExtensions.hpp>\n")

add_library(context_extensions_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/header.cpp")

target_link_libraries(context_extensions_header PRIVATE lux::engine::engine_context)

foreach(_target context_extensions context_extensions_header)
    if(MSVC)
        target_compile_options(${_target} PRIVATE /permissive- /GR- /utf-8)
    else()
        target_compile_options(${_target} PRIVATE -fno-rtti)
    endif()
endforeach()
