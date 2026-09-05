cmake_minimum_required(VERSION 3.22)
project(lux_cpp_coroutine_script_consumer LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-core REQUIRED COMPONENTS type_info)
find_package(lux-engine-function REQUIRED COMPONENTS script_artifact script_core)
find_package(lux-engine-core REQUIRED COMPONENTS task)
find_package(lux-engine-physics2d REQUIRED COMPONENTS physics2d_simulation)
find_package(lux-engine-simulation-composition REQUIRED COMPONENTS simulation_composition)
find_package(
    lux-engine-simulation
    REQUIRED COMPONENTS
        simulation_script
        simulation_scripting_core
        simulation_script_cpp_static
)

include_component_cmake_scripts(lux::engine::simulation::simulation_script_cpp_static)

add_executable(lux_cpp_coroutine_script_consumer main.cpp)
target_include_directories(lux_cpp_coroutine_script_consumer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(lux_cpp_coroutine_script_consumer PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(lux_cpp_coroutine_script_consumer PRIVATE /UNDEBUG)
else()
    target_compile_options(lux_cpp_coroutine_script_consumer PRIVATE -UNDEBUG)
endif()
target_link_libraries(
    lux_cpp_coroutine_script_consumer
    PRIVATE
        lux::engine::core::type_info
        lux::engine::function::script_artifact
        lux::engine::physics2d::physics2d_simulation
        lux::engine::simulation::simulation_composition
        lux::engine::simulation::simulation_script_cpp_static
        lux::engine::core::task
)
if(CMAKE_CROSSCOMPILING)
    message(FATAL_ERROR "This consumer needs a target-produced Event schema; host execution is not a substitute")
endif()
add_executable(consumer_event_schema_writer EventSchemaWriter.cpp)
target_compile_features(consumer_event_schema_writer PRIVATE cxx_std_20)
target_link_libraries(consumer_event_schema_writer PRIVATE
    lux::engine::simulation::simulation_script lux::engine::physics2d::physics2d_simulation)
set(event_schema "${CMAKE_CURRENT_BINARY_DIR}/consumer.events.json")
add_custom_command(OUTPUT "${event_schema}" COMMAND consumer_event_schema_writer "${event_schema}"
    DEPENDS consumer_event_schema_writer VERBATIM)
add_custom_target(consumer_event_schema DEPENDS "${event_schema}")
lux_cpp_static_scripts(TARGET lux_cpp_coroutine_script_consumer NAME CoroutineBehavior
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/ConsumerBehavior.hpp LOGICAL_PATH ConsumerBehavior.hpp
    MODULE installed.cpp-coroutine DESCRIPTOR installed-cpp-coroutine-v2
    OWNER installed_consumer::CoroutineBehavior SCOPE ENTITY
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/behavior.symbols.json
    ABILITY_TYPES lux::physics2d::PhysicsQuery2D lux::simulation::script::DelayAbility
    ABILITY_HEADERS lux/engine/physics2d/abilities/PhysicsQuery2D.ability.generated.hpp
        lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp
    EVENT_SCHEMA "${event_schema}" EVENT_NAMES Gameplay.pulse DEPENDS consumer_event_schema)
