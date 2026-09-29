find_package(box2d CONFIG REQUIRED)
find_package(Eigen3 CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_PHYSICS2D_SIMULATION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_PHYSICS2D_SIMULATION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_PHYSICS2D_SIMULATION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/physics2d/visibility.h
)

include_component_cmake_scripts(meta)
include(${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/schema/cmake/engine_ecs_codegen.cmake)

add_component(
    COMPONENT_NAME physics2d_simulation
    NAMESPACE      lux::engine::physics2d
    OUTPUT_NAME    lux_engine_physics2d_simulation
    SOURCE_FILES
        src/Box2DWorld.cpp
        src/Physics2DSchema.cpp
        src/Physics2DSystem.cpp
        src/Physics2DPluginExports.cpp
)

component_include_directories(
    physics2d_simulation
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)

target_compile_definitions(
    physics2d_simulation
    PRIVATE LUX_ENGINE_PHYSICS2D_SIMULATION_LIBRARY
)
target_link_libraries(
    physics2d_simulation
    PUBLIC
        lux::engine::function::script_core
        lux::engine::simulation::system
        lux::engine::simulation::ecs::core
        lux::engine::simulation::ecs::transform
        Eigen3::Eigen
    PRIVATE
        lux::engine::core::serialization
        lux::engine::simulation::simulation_composition
        box2d::box2d
)
component_add_transitive_commands(
    physics2d_simulation
    "find_package(Eigen3 CONFIG REQUIRED)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core transform)"
)

engine_target_add_ecs_component_codegen(
    NAME physics2d_component_codegen
    TARGET physics2d_simulation
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/Physics2DSchema.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/physics2d/Physics2DComponents.hpp
    LOGICAL_PATH lux/engine/physics2d/Physics2DComponents.hpp
    SYMBOL physics2d
)
engine_target_add_static_type_info(
    NAME physics2d_configuration_codegen
    TARGET physics2d_simulation
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/Physics2DSystem.cpp
    HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/physics2d/Physics2DSystem.hpp
    LOGICAL_PATHS lux/engine/physics2d/Physics2DSystem.hpp
)
lux_script_abilities(
    TARGET physics2d_simulation
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/physics2d/abilities/PhysicsQuery2D.hpp
    LOGICAL_PATHS PhysicsQuery2D.hpp
)
get_target_property(_physics2d_ability_generated_dir physics2d_simulation LUX_SCRIPT_ABILITY_GENERATED_DIR)
install(
    FILES ${_physics2d_ability_generated_dir}/PhysicsQuery2D.ability.generated.hpp
    DESTINATION include/lux/engine/physics2d/abilities
    COMPONENT lux_sdk
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-physics2d
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::physics2d
    COMPONENTS   physics2d_simulation
)

find_package(nlohmann_json CONFIG REQUIRED)
add_executable(physics2d_description src/Physics2DDescription.cpp)
lux_classify_target(TARGET physics2d_description LAYER TOOLCHAIN PRODUCT SDK ROLE BUILD_TOOL)
target_compile_definitions(physics2d_description PRIVATE LUX_ENGINE_PHYSICS2D_SIMULATION_DLL_DISABLE)
target_include_directories(physics2d_description PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LUX_GENERATE_HEADER_DIR} ${CMAKE_CURRENT_BINARY_DIR}/type_info)
target_link_libraries(physics2d_description PRIVATE nlohmann_json::nlohmann_json
    lux::engine::simulation::system lux::engine::simulation::ecs::transform
    lux::engine::function::script_core lux::engine::core::serialization)
# Codegen labels are metadata-only targets; compilation requires the output-producing targets.
add_dependencies(physics2d_description physics2d_component_codegen_generate physics2d_configuration_codegen_generate)
set(_physics_declaration "${CMAKE_CURRENT_BINARY_DIR}/physics2d.declaration.json")
add_custom_command(OUTPUT "${_physics_declaration}"
    COMMAND "$<TARGET_FILE:physics2d_description>" "${CMAKE_CURRENT_SOURCE_DIR}/physics2d.plugin.json" "${_physics_declaration}"
    DEPENDS physics2d_description physics2d.plugin.json VERBATIM)
add_custom_target(physics2d_declaration DEPENDS "${_physics_declaration}")
