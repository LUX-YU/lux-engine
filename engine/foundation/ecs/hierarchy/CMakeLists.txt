generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_HIERARCHY_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_HIERARCHY_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_HIERARCHY_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/hierarchy/visibility.h
)

add_component(
    COMPONENT_NAME ecs_hierarchy
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_hierarchy
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/HierarchyIndex.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/HierarchySchema.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/HierarchyMaintenance.cpp
)

component_include_directories(
    ecs_hierarchy
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

target_compile_definitions(ecs_hierarchy PRIVATE LUX_ENGINE_ECS_HIERARCHY_LIBRARY)
target_link_libraries(
    ecs_hierarchy
    PUBLIC
        lux::engine::ecs::ecs_core
        lux::engine::ecs::ecs_schema
        lux::engine::ecs::ecs_snapshot
)
component_add_internal_dependencies(ecs_hierarchy ecs_core ecs_schema ecs_snapshot)
component_add_transitive_commands(
    ecs_hierarchy
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_core ecs_schema ecs_snapshot)"
)

include_component_cmake_scripts(meta)
include(${PROJECT_SOURCE_DIR}/engine/foundation/ecs/schema/cmake/engine_ecs_codegen.cmake)
engine_target_add_ecs_component_codegen(
    NAME hierarchy_component_codegen
    TARGET ecs_hierarchy
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/HierarchySchema.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/Parent.hpp
    LOGICAL_PATH lux/engine/ecs/Parent.hpp
    SYMBOL hierarchy
)
