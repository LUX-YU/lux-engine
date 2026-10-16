cmake_minimum_required(VERSION 3.22)

project(render-protocol-installed-consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /GR- /we4541)
endif()

enable_testing()

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_protocol)

get_target_property(links lux::engine::render::render_protocol INTERFACE_LINK_LIBRARIES)
set(expected_links lux::engine::render::render_core lux::cxx::core STDEXEC::stdexec)

if(NOT links STREQUAL expected_links)
    message(FATAL_ERROR "Protocol dependency leak: ${links}")
endif()

if(TARGET render_connection OR TARGET lux::engine::render::render_connection)
    message(FATAL_ERROR "Private Connection installed")
endif()

if(TARGET lux::engine::render::render_data OR TARGET lux::engine::platform::platform_ipc)
    message(FATAL_ERROR "Pure Protocol consumer loaded Data or IPC")
endif()

add_executable(protocol_consumer Main.cpp)

target_link_libraries(protocol_consumer PRIVATE lux::engine::render::render_protocol)

add_test(NAME installed.protocol COMMAND protocol_consumer)
