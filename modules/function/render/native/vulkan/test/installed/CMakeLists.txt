cmake_minimum_required(VERSION 3.22)

project(render-native-installed-consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /GR-)
endif()

enable_testing()

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_native_vulkan)

add_executable(native_consumer Main.cpp)

target_link_libraries(native_consumer PRIVATE lux::engine::render::render_native_vulkan)

add_test(NAME installed.native.compiler COMMAND native_consumer)
