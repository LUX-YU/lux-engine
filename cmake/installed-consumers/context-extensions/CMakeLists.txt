cmake_minimum_required(VERSION 3.28)

project(EngineAssemblyConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(lux-engine-runtime REQUIRED COMPONENTS engine_runtime)

enable_testing()

add_executable(engine_assembly "${CMAKE_CURRENT_LIST_DIR}/../../../engine/runtime/engine/test/assembly.cpp")

target_link_libraries(engine_assembly PRIVATE lux::engine::engine_runtime)

add_test(NAME installed.engine_assembly COMMAND engine_assembly)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/header.cpp" "#include <lux/engine/EngineAssembly.hpp>\n")

add_library(engine_assembly_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/header.cpp")

target_link_libraries(engine_assembly_header PRIVATE lux::engine::engine_runtime)

foreach(_target engine_assembly engine_assembly_header)
    if(MSVC)
        target_compile_options(${_target} PRIVATE /permissive- /GR- /utf-8)
    else()
        target_compile_options(${_target} PRIVATE -fno-rtti)
    endif()
endforeach()
