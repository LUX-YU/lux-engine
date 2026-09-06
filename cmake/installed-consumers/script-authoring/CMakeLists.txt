cmake_minimum_required(VERSION 3.22)
project(lux_script_authoring_consumer LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-core REQUIRED COMPONENTS type_info task)
find_package(lux-engine-function REQUIRED COMPONENTS script_core script_artifact script_native)
find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_script simulation_script_cpp_static
    simulation_script_lua simulation_script_native)
find_package(lux-engine-simulation-composition REQUIRED COMPONENTS simulation_composition)
find_package(lux-engine-toolchain REQUIRED COMPONENTS lua_script_packager)
find_package(lux-engine-flowforge-compiler REQUIRED COMPONENTS flowforge_compiler)
find_package(nlohmann_json CONFIG REQUIRED)
include_component_cmake_scripts(lux::engine::function::script_core)
include_component_cmake_scripts(lux::engine::simulation::simulation_script_cpp_static)
include_component_cmake_scripts(lux::engine::toolchain::lua_script_packager)

add_library(authoring_counter STATIC CounterAbility.cpp)
target_include_directories(authoring_counter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(authoring_counter PUBLIC cxx_std_20)
target_link_libraries(authoring_counter PUBLIC lux::engine::function::script_core)
lux_script_abilities(TARGET authoring_counter SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/CounterAbility.hpp
    LOGICAL_PATHS CounterAbility.hpp)

add_executable(authoring_graph_writer GraphWriter.cpp)
target_compile_features(authoring_graph_writer PRIVATE cxx_std_20)
target_link_libraries(authoring_graph_writer PRIVATE authoring_counter
    lux::engine::flowforge::flowforge_compiler nlohmann_json::nlohmann_json)
set(flow_artifact ${CMAKE_CURRENT_BINARY_DIR}/flow.lxsa)
set(flow_hints ${CMAKE_CURRENT_BINARY_DIR}/flow.bindings.json)
add_custom_command(OUTPUT ${flow_artifact} ${flow_hints}
    COMMAND ${CMAKE_COMMAND} -E env "LUX_FLOWFORGE_LINKER=${CMAKE_LINKER}"
        $<TARGET_FILE:authoring_graph_writer> ${flow_artifact} ${flow_hints}
    DEPENDS authoring_graph_writer VERBATIM)
add_custom_target(authoring_flow_artifact DEPENDS ${flow_artifact} ${flow_hints})

set(lua_artifact ${CMAKE_CURRENT_BINARY_DIR}/lua.lxsa)
lux_package_lua_script(NAME authoring_lua_artifact SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/behavior.lua
    OUTPUT ${lua_artifact} MODULE consumer.authoring.lua ENTRY Common SCOPE ENTITY
    ASSET_ID 00000000-0000-4000-8000-000000000032
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/lua.symbols.json ABILITY_TARGETS authoring_counter)

add_executable(lux_script_authoring_consumer main.cpp)
target_compile_features(lux_script_authoring_consumer PRIVATE cxx_std_20)
target_link_libraries(lux_script_authoring_consumer PRIVATE authoring_counter nlohmann_json::nlohmann_json
    lux::engine::core::type_info lux::engine::core::task
    lux::engine::simulation::simulation_composition lux::engine::simulation::simulation_script_cpp_static
    lux::engine::simulation::simulation_script_lua lux::engine::simulation::simulation_script_native)
lux_cpp_static_scripts(TARGET lux_script_authoring_consumer NAME CommonBehavior
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/Behavior.hpp LOGICAL_PATH Behavior.hpp
    MODULE consumer.authoring.cpp DESCRIPTOR consumer-authoring-cpp OWNER authoring_consumer::Behavior SCOPE ENTITY
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/cpp.symbols.json OUTPUT_ROOT ${CMAKE_CURRENT_BINARY_DIR}/cpp)
target_compile_definitions(lux_script_authoring_consumer PRIVATE
    LUA_ARTIFACT="${lua_artifact}" FLOW_ARTIFACT="${flow_artifact}"
    CPP_HINTS="${CMAKE_CURRENT_BINARY_DIR}/cpp/Behavior.CommonBehavior.bindings.json"
    LUA_HINTS="${lua_artifact}.bindings.json" FLOW_HINTS="${flow_hints}")
add_dependencies(lux_script_authoring_consumer authoring_lua_artifact authoring_flow_artifact)

find_package(lux-engine-scene-script-description REQUIRED COMPONENTS scene_script_description)
target_link_libraries(lux_script_authoring_consumer PRIVATE lux::engine::scene::scene_script_description)
