cmake_minimum_required(VERSION 3.25)

project(lux_headless_engine_consumer LANGUAGES CXX)

find_package(lux-engine-initialization CONFIG REQUIRED COMPONENTS engine_initialization)
find_package(lux-engine-runtime-metadata CONFIG REQUIRED COMPONENTS engine_metadata)
find_package(lux-engine-scene-transform CONFIG REQUIRED COMPONENTS system_transform)

add_executable(headless_engine main.cpp)

target_compile_features(headless_engine PRIVATE cxx_std_20)

target_link_libraries(
    headless_engine
    PRIVATE
        lux::engine::engine_initialization
        lux::engine::engine_metadata
        lux::engine::scene::system_transform
)

if(MSVC)
    target_compile_options(headless_engine PRIVATE /GR- /utf-8)
else()
    target_compile_options(headless_engine PRIVATE -fno-rtti)
endif()

enable_testing()

add_test(NAME installed.headless_engine COMMAND headless_engine)
set_tests_properties(installed.headless_engine PROPERTIES TIMEOUT 30)
