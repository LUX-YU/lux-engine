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
include_component_cmake_scripts(lux::engine::function::script_core)

# Reproject the installed canonical declaration. The runtime SDK is not assumed to contain
# schema JSON installed by a different TOOLCHAIN prefix.
get_target_property(physics_includes lux::engine::physics2d::physics2d_simulation INTERFACE_INCLUDE_DIRECTORIES)
find_file(physics_declaration lux/engine/physics2d/abilities/PhysicsQuery2D.hpp
    PATHS ${physics_includes} NO_DEFAULT_PATH REQUIRED)
add_library(physics_schema_declaration STATIC SchemaDeclaration.cpp)
target_compile_features(physics_schema_declaration PRIVATE cxx_std_20)
target_include_directories(physics_schema_declaration PRIVATE ${physics_includes})
target_link_libraries(physics_schema_declaration PRIVATE lux::engine::function::script_core)
lux_script_abilities(TARGET physics_schema_declaration SOURCES "${physics_declaration}"
    LOGICAL_PATHS PhysicsQuery2D.hpp)

set(physics_lua ${CMAKE_CURRENT_BINARY_DIR}/physics.lxsa)
lux_package_lua_script(
    NAME physics_lua_artifact
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/physics.lua
    OUTPUT ${physics_lua}
    ABILITY_TARGETS physics_schema_declaration
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
