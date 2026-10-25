cmake_minimum_required(VERSION 3.28)

project(render_extension_consumer LANGUAGES CXX)

find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_extensions)

add_executable(render_extension Probe.cpp)
target_link_libraries(render_extension PRIVATE lux::engine::function::render_extensions)
target_compile_features(render_extension PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(render_extension PRIVATE /utf-8 /permissive- /Zc:__cplusplus /GR- /bigobj)
endif()

enable_testing()
add_test(NAME installed.render.extension COMMAND render_extension)
set_tests_properties(installed.render.extension PROPERTIES TIMEOUT 45)
