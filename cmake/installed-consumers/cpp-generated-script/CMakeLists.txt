cmake_minimum_required(VERSION 3.22)
project(lux_cpp_generated_script_consumer LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-core REQUIRED COMPONENTS type_info task)
find_package(lux-engine-simulation-composition REQUIRED COMPONENTS simulation_composition)
find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script simulation_script_cpp_static)
include_component_cmake_scripts(lux::engine::simulation::simulation_script_cpp_static)
add_executable(lux_cpp_generated_script_consumer main.cpp)
target_compile_features(lux_cpp_generated_script_consumer PRIVATE cxx_std_20)
target_include_directories(lux_cpp_generated_script_consumer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lux_cpp_generated_script_consumer PRIVATE lux::engine::core::type_info lux::engine::core::task
    lux::engine::simulation::simulation_composition lux::engine::simulation::simulation_script_cpp_static)
lux_cpp_static_scripts(TARGET lux_cpp_generated_script_consumer NAME InstalledBehavior
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/Behavior.hpp LOGICAL_PATH Behavior.hpp
    MODULE installed.generated DESCRIPTOR installed-generated-v1 OWNER installed_generated::Behavior SCOPE ENTITY
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/behavior.symbols.json
    ABILITY_TYPES lux::simulation::script::DelayAbility
    ABILITY_HEADERS lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp)
