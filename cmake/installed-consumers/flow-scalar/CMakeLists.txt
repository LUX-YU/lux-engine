cmake_minimum_required(VERSION 3.28)

project(FlowScalarConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(lux-engine-flowforge-compiler REQUIRED COMPONENTS flowforge_compiler)

enable_testing()

add_executable(scalar_lowering "${CMAKE_CURRENT_LIST_DIR}/../../../engine/toolchain/flowforge/test/scalar_lowering.cpp")

target_link_libraries(scalar_lowering PRIVATE lux::engine::flowforge::flowforge_compiler)

if(MSVC)
    target_compile_options(scalar_lowering PRIVATE /permissive- /GR- /utf-8)
else()
    target_compile_options(scalar_lowering PRIVATE -fno-rtti)
endif()

add_test(NAME installed.flow_scalar_lowering COMMAND scalar_lowering)

add_executable(control_lowering "${CMAKE_CURRENT_LIST_DIR}/../../../engine/toolchain/flowforge/test/control_lowering.cpp")

target_link_libraries(control_lowering PRIVATE lux::engine::flowforge::flowforge_compiler)

add_test(NAME installed.flow_control_compiler_compatibility COMMAND control_lowering)
