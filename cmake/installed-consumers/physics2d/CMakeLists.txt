cmake_minimum_required(VERSION 3.24)
project(physics2d_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(lux-engine-physics2d REQUIRED COMPONENTS physics2d_simulation)
find_package(lux-engine-simulation-composition REQUIRED COMPONENTS simulation_composition)
find_package(box2d CONFIG REQUIRED)
add_executable(physics2d_consumer main.cpp)
target_link_libraries(
    physics2d_consumer PRIVATE
    lux::engine::physics2d::physics2d_simulation
    lux::engine::simulation::simulation_composition
    box2d::box2d
)
if(MSVC)
    target_compile_options(physics2d_consumer PRIVATE /UNDEBUG /permissive- /utf-8 /GR- /we4541)
else()
    target_compile_options(physics2d_consumer PRIVATE -UNDEBUG -fno-rtti)
endif()
enable_testing()
add_test(NAME installed.physics2d.lifecycle COMMAND physics2d_consumer)
set_tests_properties(installed.physics2d.lifecycle PROPERTIES TIMEOUT 30)
