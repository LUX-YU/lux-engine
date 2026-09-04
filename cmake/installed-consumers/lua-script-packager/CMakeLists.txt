cmake_minimum_required(VERSION 3.22)
project(lux_lua_script_packager_consumer LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-function REQUIRED COMPONENTS script_core script_artifact)
find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script_lua)
find_package(lux-engine-toolchain REQUIRED COMPONENTS lua_script_packager)

include_component_cmake_scripts(lux::engine::function::script_core)
include_component_cmake_scripts(lux::engine::toolchain::lua_script_packager)

add_library(inventory_ability_contract STATIC InventoryAbility.cpp)
target_include_directories(inventory_ability_contract PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(inventory_ability_contract PUBLIC cxx_std_20)
target_link_libraries(inventory_ability_contract PUBLIC lux::engine::function::script_core)
lux_script_abilities(
    TARGET inventory_ability_contract
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/InventoryAbility.hpp
    LOGICAL_PATHS InventoryAbility.hpp
)

add_executable(inventory_event_schema_writer EventSchemaWriter.cpp)
target_compile_features(inventory_event_schema_writer PRIVATE cxx_std_20)
target_link_libraries(
    inventory_event_schema_writer
    PRIVATE lux::engine::toolchain::lua_script_packager
)
set(event_schema ${CMAKE_CURRENT_BINARY_DIR}/inventory.events.schema.json)
add_custom_command(
    OUTPUT ${event_schema}
    COMMAND $<TARGET_FILE:inventory_event_schema_writer> ${event_schema}
    DEPENDS inventory_event_schema_writer
    VERBATIM
)
add_custom_target(inventory_event_schema DEPENDS ${event_schema})

set(artifact ${CMAKE_CURRENT_BINARY_DIR}/inventory.lxsa)
lux_package_lua_script(
    NAME inventory_lua_artifact
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/inventory.lua
    OUTPUT ${artifact}
    VALUE_TYPES lux.simulation.ScriptEndPlayReason,3,4,4
    ABILITY_TARGETS inventory_ability_contract
    EVENT_SCHEMAS ${event_schema}
    EVENT_SCHEMA_TARGETS inventory_event_schema
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/inventory.symbols.json
    MODULE consumer.inventory.lua-script
    ENTRY InventoryBehavior
    SCOPE ENTITY
)

add_executable(lua_script_packager_consumer main.cpp)
target_compile_features(lua_script_packager_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lua_script_packager_consumer
    PRIVATE
        inventory_ability_contract
        lux::engine::function::script_artifact
        lux::engine::simulation::simulation_script_lua
)
target_compile_definitions(lua_script_packager_consumer PRIVATE LUX_LUA_ARTIFACT="${artifact}")
add_dependencies(lua_script_packager_consumer inventory_lua_artifact)
