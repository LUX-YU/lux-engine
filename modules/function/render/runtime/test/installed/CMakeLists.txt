cmake_minimum_required(VERSION 3.22)
project(render-runtime-installed-consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /GR-)
endif()

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_client render_runtime)

add_executable(client_consumer Client.cpp)
target_link_libraries(client_consumer PRIVATE lux::engine::render::render_client)
add_executable(runtime_consumer Main.cpp)
target_link_libraries(runtime_consumer PRIVATE lux::engine::render::render_runtime)

foreach(consumer client_consumer runtime_consumer)
    add_custom_command(TARGET ${consumer} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:lux::engine::platform::platform_ipc>" "$<TARGET_FILE_DIR:${consumer}>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:lux::engine::render::render_client>" "$<TARGET_FILE_DIR:${consumer}>"
    )
    add_test(NAME installed.runtime.${consumer} COMMAND ${consumer})
endforeach()

add_executable(extension_consumer ../BufferExtension.cpp ../../../native/vulkan/test/Allocation.cpp)

target_include_directories(extension_consumer PRIVATE ../../../native/vulkan/test)

target_link_libraries(extension_consumer PRIVATE lux::engine::render::render_runtime)

add_custom_command(TARGET extension_consumer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:lux::engine::platform::platform_ipc>" "$<TARGET_FILE_DIR:extension_consumer>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:lux::engine::render::render_client>" "$<TARGET_FILE_DIR:extension_consumer>"
)

add_test(NAME installed.runtime.external_storage COMMAND extension_consumer)

set_tests_properties(installed.runtime.external_storage PROPERTIES TIMEOUT 60)
