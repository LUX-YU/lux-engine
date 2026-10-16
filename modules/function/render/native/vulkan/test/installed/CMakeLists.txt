cmake_minimum_required(VERSION 3.22)

project(render-native-installed-consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /GR-)
endif()

enable_testing()

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_native_vulkan)
find_package(lux-engine-process-execution CONFIG REQUIRED COMPONENTS process_execution)

add_executable(native_consumer Main.cpp)

target_link_libraries(native_consumer PRIVATE lux::engine::render::render_native_vulkan)

add_test(NAME installed.native.compiler COMMAND native_consumer)

add_executable(texture_consumer Texture.cpp)

target_link_libraries(texture_consumer PRIVATE lux::engine::render::render_native_vulkan
    lux::engine::process::process_execution
)

foreach(target lux::engine::process::process_execution lux::engine::core::error)
    add_custom_command(TARGET texture_consumer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${target}>" "$<TARGET_FILE_DIR:texture_consumer>"
    )
endforeach()

add_test(NAME installed.native.texture COMMAND texture_consumer)
