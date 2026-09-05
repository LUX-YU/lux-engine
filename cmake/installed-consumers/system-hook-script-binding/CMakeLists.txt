cmake_minimum_required(VERSION 3.22)
project(lux_system_hook_script_consumer LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-core REQUIRED COMPONENTS type_info)
find_package(lux-engine-simulation-composition REQUIRED COMPONENTS simulation_composition)
find_package(lux-engine-function REQUIRED COMPONENTS script_artifact script_core)
find_package(
    lux-engine-simulation
    REQUIRED COMPONENTS
        simulation_description
        simulation_script
        simulation_scripting_core
        simulation_script_cpp_static
)
find_package(
    lux-engine-simulation-asset
    REQUIRED COMPONENTS simulation_asset
)

include_component_cmake_scripts(lux::engine::simulation::simulation_script_cpp_static)
include_component_cmake_scripts(lux::engine::function::script_core)
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
        lux::engine::function::script_artifact
        lux::engine::simulation::description
        lux::engine::simulation::simulation_script
        lux::engine::simulation::simulation_script_cpp_static
        lux::engine::simulation::asset
        lux::engine::simulation::composition
)
target_link_libraries(lux_system_hook_script_consumer PRIVATE lux::engine::core::type_info)
lux_script_abilities(TARGET lux_system_hook_script_consumer
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/InventoryAbility.hpp LOGICAL_PATHS InventoryAbility.hpp)
lux_cpp_static_scripts(TARGET lux_system_hook_script_consumer NAME HookBehavior
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/ConsumerBehavior.hpp LOGICAL_PATH ConsumerBehavior.hpp
    MODULE consumer.behavior DESCRIPTOR consumer-behavior-v1 OWNER installed_consumer::ConsumerBehavior SCOPE ENTITY
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/behavior.symbols.json
    ABILITY_TYPES installed_consumer::InventoryAbility ABILITY_HEADERS InventoryAbility.ability.generated.hpp
    DEPENDS lux_system_hook_script_consumer_script_abilities_generate)
