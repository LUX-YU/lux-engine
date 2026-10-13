cmake_minimum_required(VERSION 3.22)

project(render-data-installed-consumer LANGUAGES CXX)

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

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_data)

add_executable(data_consumer Main.cpp)

target_link_libraries(data_consumer PRIVATE lux::engine::render::render_data)

add_test(NAME installed.data COMMAND data_consumer)

foreach(header Schema Wire)
    string(CONCAT probe
        "#include <lux/engine/render/data/${header}.hpp>\nstatic_assert(__cplusplus >= 202002L);\n"
        "#if defined(VK_VERSION_1_0) || defined(_WINDOWS_)\n#error Native header leaked into Data\n#endif\n"
    )

    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp" CONTENT "${probe}")

    add_library(installed_data_${header} OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${header}.cpp")

    target_link_libraries(installed_data_${header} PRIVATE lux::engine::render::render_data)
endforeach()

get_target_property(data_links lux::engine::render::render_data INTERFACE_LINK_LIBRARIES)
set(expected_links lux::engine::render::render_core lux::cxx::core lux::cxx::binary lux::cxx::memory)
if(NOT data_links STREQUAL expected_links)
    message(FATAL_ERROR "Unexpected installed Data dependency: ${data_links}")
endif()
