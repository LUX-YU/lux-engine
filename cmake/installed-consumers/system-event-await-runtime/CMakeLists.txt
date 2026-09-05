cmake_minimum_required(VERSION 3.22)
project(lux_system_event_await_runtime_consumer LANGUAGES CXX)

find_package(lux-engine-function REQUIRED COMPONENTS script_artifact script_core)
find_package(lux-engine-simulation-composition REQUIRED COMPONENTS simulation_composition)
find_package(
    lux-engine-simulation
    REQUIRED COMPONENTS
        simulation_description
        simulation_script
        simulation_scripting_core
)

add_executable(lux_system_event_await_runtime_consumer main.cpp)
target_compile_features(lux_system_event_await_runtime_consumer PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(lux_system_event_await_runtime_consumer PRIVATE /UNDEBUG)
else()
    target_compile_options(lux_system_event_await_runtime_consumer PRIVATE -UNDEBUG)
endif()
target_link_libraries(
    lux_system_event_await_runtime_consumer
    PRIVATE
        lux::engine::function::script_artifact
        lux::engine::simulation::simulation_script
        lux::engine::simulation::composition
)
