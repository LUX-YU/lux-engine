cmake_minimum_required(VERSION 3.22)
project(lux_script_authoring_consumer LANGUAGES CXX)

find_package(lux-engine-authoring REQUIRED COMPONENTS authoring_script)
find_package(
    lux-engine-simulation
    REQUIRED COMPONENTS simulation_description simulation_script simulation_scripting
)
find_package(
    lux-engine-simulation-asset
    REQUIRED COMPONENTS simulation_asset
)

add_executable(lux_script_authoring_consumer main.cpp)
target_compile_features(lux_script_authoring_consumer PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(lux_script_authoring_consumer PRIVATE /UNDEBUG)
else()
    target_compile_options(lux_script_authoring_consumer PRIVATE -UNDEBUG)
endif()
target_link_libraries(
    lux_script_authoring_consumer
    PRIVATE
        lux::engine::authoring::authoring_script
        lux::engine::simulation::description
        lux::engine::simulation::simulation_script
        lux::engine::simulation::asset
)
