cmake_minimum_required(VERSION 3.22)
project(lux_native_task_lua_steps_consumer LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-core REQUIRED COMPONENTS type_info)
find_package(lux-engine-function REQUIRED COMPONENTS script_artifact script_core)
find_package(lux-engine-core REQUIRED COMPONENTS task)
find_package(lux-engine-simulation-composition REQUIRED COMPONENTS simulation_composition)
find_package(
    lux-engine-simulation
    REQUIRED COMPONENTS
        simulation_script
        simulation_scripting_core
        simulation_script_cpp_static
        simulation_script_native_lua_tasks
        simulation_script_lua
)

include_component_cmake_scripts(lux::engine::simulation::simulation_script_cpp_static)

add_executable(lux_native_task_lua_steps_consumer main.cpp)
target_include_directories(lux_native_task_lua_steps_consumer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(lux_native_task_lua_steps_consumer PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(lux_native_task_lua_steps_consumer PRIVATE /UNDEBUG)
else()
    target_compile_options(lux_native_task_lua_steps_consumer PRIVATE -UNDEBUG)
endif()
target_link_libraries(
    lux_native_task_lua_steps_consumer
    PRIVATE
        lux::engine::core::type_info
        lux::engine::function::script_artifact
        lux::engine::simulation::simulation_composition
        lux::engine::simulation::simulation_script_native_lua_tasks
        lux::engine::core::task
)
if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "This consumer needs a target-produced Event schema; host execution is not a substitute")
endif()
add_executable(consumer_event_schema_writer EventSchemaWriter.cpp)
target_compile_features(consumer_event_schema_writer PRIVATE cxx_std_20)
target_link_libraries(consumer_event_schema_writer PRIVATE
    lux::engine::simulation::simulation_script)
set(event_schema "${CMAKE_CURRENT_BINARY_DIR}/consumer.events.json")
add_custom_command(OUTPUT "${event_schema}" COMMAND consumer_event_schema_writer "${event_schema}"
    DEPENDS consumer_event_schema_writer VERBATIM)
add_custom_target(consumer_event_schema DEPENDS "${event_schema}")
lux_cpp_static_scripts(TARGET lux_native_task_lua_steps_consumer NAME CoroutineBehavior
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/ConsumerBehavior.hpp LOGICAL_PATH ConsumerBehavior.hpp
    MODULE installed.cpp-coroutine DESCRIPTOR installed-cpp-coroutine-v2
    OWNER installed_consumer::CoroutineBehavior SCOPE ENTITY
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/behavior.symbols.json
    ABILITY_TYPES lux::simulation::script::DelayAbility
    ABILITY_HEADERS lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp
    EVENT_SCHEMA "${event_schema}" EVENT_NAMES Gameplay.pulse DEPENDS consumer_event_schema)

find_package(lux-engine-toolchain REQUIRED COMPONENTS lua_script_packager)
include_component_cmake_scripts(lux::engine::function::script_core)
include_component_cmake_scripts(lux::engine::toolchain::lua_script_packager)
add_library(probe_ability_contract STATIC ProbeAbility.cpp)
target_compile_features(probe_ability_contract PUBLIC cxx_std_20)
target_include_directories(probe_ability_contract PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(probe_ability_contract PUBLIC lux::engine::simulation::simulation_scripting_core)
lux_script_abilities(TARGET probe_ability_contract SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/ProbeAbility.hpp)
find_path(delay_include NAMES lux/engine/simulation/abilities/DelayAbility.hpp REQUIRED)
add_library(installed_delay_contract STATIC DelayImports.cpp)
target_compile_features(installed_delay_contract PUBLIC cxx_std_20)
target_link_libraries(installed_delay_contract PUBLIC lux::engine::simulation::simulation_scripting_core)
lux_script_abilities(TARGET installed_delay_contract
    SOURCES ${delay_include}/lux/engine/simulation/abilities/DelayAbility.hpp
    LOGICAL_PATHS lux/engine/simulation/abilities/DelayAbility.hpp)
set(artifact ${CMAKE_CURRENT_BINARY_DIR}/steps.lxsa)
lux_package_lua_script(NAME native_lua_steps_artifact SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/steps.lua
    OUTPUT ${artifact} MODULE installed.native-lua.steps ENTRY Steps SCOPE ENTITY
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/steps.symbols.json
    SEMANTIC_TYPES lux::simulation::script::EScriptEndPlayReason
    SEMANTIC_HEADERS lux/engine/simulation/scripting/ScriptLifecycle.hpp
    SEMANTIC_TARGETS lux::engine::simulation::simulation_scripting_core
    ABILITY_TARGETS probe_ability_contract installed_delay_contract
    EVENT_SCHEMAS ${event_schema} EVENT_SCHEMA_TARGETS consumer_event_schema)
target_link_libraries(lux_native_task_lua_steps_consumer PRIVATE probe_ability_contract installed_delay_contract)
target_compile_definitions(lux_native_task_lua_steps_consumer PRIVATE LUX_LUA_ARTIFACT="${artifact}")
add_dependencies(lux_native_task_lua_steps_consumer native_lua_steps_artifact)
