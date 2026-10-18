cmake_minimum_required(VERSION 3.22)

project(render-core-installed-consumer LANGUAGES CXX)

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

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_core)

add_executable(core_consumer Main.cpp)

target_link_libraries(core_consumer PRIVATE lux::engine::render::render_core)

add_test(NAME installed.core COMMAND core_consumer)

foreach(header Ids Error Target RasterViewport Execution)
    string(CONCAT header_probe
        "#include <lux/engine/render/core/${header}.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
        "#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native header leaked into Core\n#endif\n"
    )

    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp" CONTENT "${header_probe}")

    add_library(installed_header_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp")

    target_link_libraries(installed_header_${header} PRIVATE lux::engine::render::render_core)
endforeach()

get_target_property(core_links lux::engine::render::render_core INTERFACE_LINK_LIBRARIES)
set(expected_links lux::cxx::container lux::cxx::compile_time lux::cxx::algorithm)
if(NOT core_links STREQUAL expected_links)
    message(FATAL_ERROR "Unexpected installed Core dependency: ${core_links}")
endif()
