cmake_minimum_required(VERSION 3.22)

project(render-transport-installed-consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /GR- /we4541)
else()
    add_compile_options(-fno-rtti)
endif()

enable_testing()

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_transport)

get_target_property(transport_links lux::engine::render::render_transport INTERFACE_LINK_LIBRARIES)

set(expected_links lux::engine::render::render_data lux::cxx::container "$<LINK_ONLY:lux::cxx::concurrent>")

if(NOT transport_links STREQUAL expected_links)
    message(FATAL_ERROR "Unexpected installed Transport dependency: ${transport_links}")
endif()

add_executable(transport_consumer Main.cpp)

target_link_libraries(transport_consumer PRIVATE lux::engine::render::render_transport)

add_test(NAME installed.transport COMMAND transport_consumer)

foreach(header Operation Routes Replies Transport)
    string(CONCAT probe
        "#include <lux/engine/render/transport/${header}.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
        "#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native header leaked into Transport\n#endif\n"
    )

    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp" CONTENT "${probe}")

    add_library(installed_transport_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp")

    target_link_libraries(installed_transport_${header} PRIVATE lux::engine::render::render_transport)
endforeach()
