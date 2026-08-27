cmake_minimum_required(VERSION 3.22)
project(lux_system_hook_script_binding_consumer LANGUAGES CXX)

find_package(lux-engine-core REQUIRED COMPONENTS script_meta_adapter)
find_package(lux-engine-function REQUIRED COMPONENTS script_core)
find_package(lux-engine-resource-script REQUIRED COMPONENTS script_asset)
find_package(
    lux-engine-simulation
    REQUIRED COMPONENTS simulation_description simulation_script_binding
)
find_package(
    lux-engine-simulation-asset
    REQUIRED COMPONENTS simulation_asset
)

add_executable(lux_system_hook_script_binding_consumer main.cpp)
target_compile_features(
    lux_system_hook_script_binding_consumer
    PRIVATE cxx_std_20
)
target_link_libraries(
    lux_system_hook_script_binding_consumer
    PRIVATE
        lux::engine::core::script_meta_adapter
        lux::engine::function::script_core
        lux::engine::resource::script_asset
        lux::engine::simulation::description
        lux::engine::simulation::simulation_script_binding
        lux::engine::simulation::asset
)
