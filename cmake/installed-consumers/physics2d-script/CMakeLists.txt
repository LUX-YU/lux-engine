cmake_minimum_required(VERSION 3.22)
project(lux_physics2d_script_consumer LANGUAGES CXX)

find_package(lux-cmake-toolset CONFIG REQUIRED)
find_package(lux-engine-function REQUIRED COMPONENTS script_artifact script_core)
find_package(lux-engine-simulation-composition REQUIRED COMPONENTS simulation_composition)
find_package(lux-engine-flowforge-compiler REQUIRED COMPONENTS flowforge_compiler)
find_package(lux-engine-toolchain REQUIRED COMPONENTS lua_script_packager)
find_package(lux-engine-physics2d REQUIRED COMPONENTS physics2d_simulation)
find_package(lux-engine-core REQUIRED COMPONENTS task)

include_component_cmake_scripts(lux::engine::toolchain::lua_script_packager)

set(physics_lua ${CMAKE_CURRENT_BINARY_DIR}/physics.lxsa)
lux_package_lua_script(
    NAME physics_lua_artifact
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/physics.lua
    OUTPUT ${physics_lua}
    ABILITY_SCHEMAS ${lux-engine-physics2d_DIR}/schema/PhysicsQuery2D.ability.schema.json
    SYMBOL_LEDGER ${CMAKE_CURRENT_SOURCE_DIR}/physics.symbols.json
    MODULE consumer.physics2d.lua
    ENTRY PhysicsConsumer
    SCOPE ENTITY
)

add_executable(lux_physics2d_script_consumer main.cpp)
target_compile_features(lux_physics2d_script_consumer PRIVATE cxx_std_20)
target_link_libraries(
    lux_physics2d_script_consumer
    PRIVATE
        lux::engine::physics2d::physics2d_simulation
        lux::engine::flowforge::flowforge_compiler
        lux::engine::simulation::simulation_composition
        lux::engine::function::script_artifact
        lux::engine::core::task
)
target_compile_definitions(
    lux_physics2d_script_consumer
    PRIVATE LUX_PHYSICS2D_LUA_ARTIFACT="${physics_lua}"
)
add_dependencies(lux_physics2d_script_consumer physics_lua_artifact)
