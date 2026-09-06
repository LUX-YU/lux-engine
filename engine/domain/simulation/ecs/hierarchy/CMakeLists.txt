generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_ECS_HIERARCHY_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_ECS_HIERARCHY_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_ECS_HIERARCHY_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/ecs/hierarchy/visibility.h
)

add_component(
    COMPONENT_NAME hierarchy
    NAMESPACE      lux::engine::simulation::ecs
    OUTPUT_NAME    lux_engine_simulation_ecs_hierarchy
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/HierarchyIndex.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/HierarchySchema.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/HierarchyMaintenance.cpp
)

component_include_directories(
    hierarchy
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    BUILD_TIME_SHARED
        ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
    INSTALL_TIME include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../core/sinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

target_compile_definitions(hierarchy PRIVATE LUX_ENGINE_SIMULATION_ECS_HIERARCHY_LIBRARY)
target_link_libraries(
    hierarchy
    PUBLIC
        lux::engine::simulation::ecs::core
        lux::engine::simulation::ecs::schema
        lux::engine::simulation::ecs::snapshot
)
component_add_internal_dependencies(hierarchy core schema snapshot)
component_add_transitive_commands(
    hierarchy
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core schema snapshot)"
)

include_component_cmake_scripts(meta)
include(${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/schema/cmake/engine_ecs_codegen.cmake)
engine_target_add_ecs_component_codegen(
    NAME hierarchy_component_codegen
    TARGET hierarchy
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/HierarchySchema.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/simulation/ecs/Parent.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Parent.hpp
    SYMBOL hierarchy
)

lux_classify_target(
    TARGET  hierarchy
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)
