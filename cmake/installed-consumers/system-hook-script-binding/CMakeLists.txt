cmake_minimum_required(VERSION 3.22)
project(lux_system_hook_script_consumer LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-core REQUIRED COMPONENTS meta)
find_package(lux-engine-function REQUIRED COMPONENTS script_core)
find_package(lux-engine-resource-script REQUIRED COMPONENTS script_asset)
find_package(
    lux-engine-simulation
    REQUIRED COMPONENTS
        simulation_description
        simulation_script
        simulation_scripting
        simulation_script_cpp_static
)
find_package(
    lux-engine-simulation-asset
    REQUIRED COMPONENTS simulation_asset
)

include_component_cmake_scripts(lux::engine::core::meta)
engine_add_type_runtime_info(
    NAME installed_script_system_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/ConsumerBehavior.hpp
    REGISTER_FUNC_NAME lux_installed_script_system
)

add_executable(lux_system_hook_script_consumer main.cpp)
target_include_directories(
    lux_system_hook_script_consumer
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_features(lux_system_hook_script_consumer PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(lux_system_hook_script_consumer PRIVATE /UNDEBUG)
else()
    target_compile_options(lux_system_hook_script_consumer PRIVATE -UNDEBUG)
endif()
target_link_libraries(
    lux_system_hook_script_consumer
    PRIVATE
        lux::engine::function::script_core
        lux::engine::resource::script_asset
        lux::engine::simulation::description
        lux::engine::simulation::simulation_script
        lux::engine::simulation::simulation_script_cpp_static
        lux::engine::simulation::asset
)
engine_target_add_type_runtime_info(
    TARGET lux_system_hook_script_consumer
    METAS installed_script_system_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen
)
