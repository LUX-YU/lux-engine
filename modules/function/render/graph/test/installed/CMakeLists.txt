cmake_minimum_required(VERSION 3.22)

project(render-graph-installed-consumer LANGUAGES CXX)

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

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_graph)

foreach(forbidden render_data render_protocol render_client render_server platform_ipc)
    if(TARGET lux::engine::render::${forbidden} OR TARGET lux::engine::platform::${forbidden})
        message(FATAL_ERROR "Graph consumer imported unrelated target ${forbidden}")
    endif()
endforeach()

add_executable(graph_consumer Main.cpp)

target_link_libraries(graph_consumer PRIVATE lux::engine::render::render_graph)

add_test(NAME installed.graph COMMAND graph_consumer)

foreach(header Graph Builder Analysis)
    string(CONCAT probe
        "#include <lux/engine/render/graph/${header}.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
        "#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native header leaked into Graph\n#endif\n"
    )

    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp" CONTENT "${probe}")

    add_library(installed_graph_header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp")

    target_link_libraries(installed_graph_header_${header} PRIVATE lux::engine::render::render_graph)
endforeach()

get_target_property(links lux::engine::render::render_graph INTERFACE_LINK_LIBRARIES)
if(NOT links STREQUAL "lux::engine::render::render_core")
    message(FATAL_ERROR "Unexpected installed Graph dependency: ${links}")
endif()
