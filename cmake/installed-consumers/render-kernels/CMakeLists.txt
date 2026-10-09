cmake_minimum_required(VERSION 3.28)

project(RenderKernelConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(lux-engine-function REQUIRED COMPONENTS render_vulkan)
find_package(lux-engine-platform REQUIRED COMPONENTS dynamic_library)

enable_testing()

set(_tests "${CMAKE_CURRENT_LIST_DIR}/../../../modules/function/render/vulkan/test")

add_executable(kernel_registration "${_tests}/kernel_registration.cpp")

target_link_libraries(kernel_registration PRIVATE lux::engine::function::render_vulkan)

add_test(NAME installed.kernel_registration COMMAND kernel_registration)

add_library(kernel_plugin SHARED "${_tests}/kernel_plugin.cpp")

target_link_libraries(kernel_plugin PRIVATE lux::engine::function::render_vulkan)

add_executable(kernel_plugin_lifetime "${_tests}/kernel_plugin_lifetime.cpp")

target_link_libraries(
    kernel_plugin_lifetime
    PRIVATE lux::engine::function::render_vulkan lux::engine::platform::dynamic_library
)

add_test(
    NAME installed.kernel_plugin_lifetime
    COMMAND kernel_plugin_lifetime "$<TARGET_FILE:kernel_plugin>"
)

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/header.cpp" "#include <lux/engine/render/graph/KernelDescriptor.hpp>\n")

add_library(kernel_public_header OBJECT "${CMAKE_CURRENT_BINARY_DIR}/header.cpp")

target_link_libraries(kernel_public_header PRIVATE lux::engine::function::render_vulkan)

foreach(_target kernel_registration kernel_plugin kernel_plugin_lifetime kernel_public_header)
    if(MSVC)
        target_compile_options(${_target} PRIVATE /permissive- /GR- /utf-8)
    else()
        target_compile_options(${_target} PRIVATE -fno-rtti)
    endif()
endforeach()
