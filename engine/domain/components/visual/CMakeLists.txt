generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_VISUAL_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_VISUAL_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_VISUAL_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/visual/visibility.h
)

add_component(
    COMPONENT_NAME ecs_visual
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_visual
    SOURCE_FILES  ${CMAKE_CURRENT_SOURCE_DIR}/src/VisualSchema.cpp
)
component_include_directories(
    ecs_visual
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(ecs_visual PRIVATE LUX_ENGINE_ECS_VISUAL_LIBRARY)
target_link_libraries(
    ecs_visual
    PUBLIC
        lux::engine::ecs::ecs_core
        lux::engine::ecs::ecs_schema
        lux::engine::ecs::ecs_snapshot
        lux::engine::resource::description
        lux::engine::core::serialization
)
component_add_internal_dependencies(ecs_visual ecs_core ecs_schema ecs_snapshot)
component_add_transitive_commands(
    ecs_visual
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS description)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_core ecs_schema ecs_snapshot)"
)

include_component_cmake_scripts(meta)
include(${PROJECT_SOURCE_DIR}/engine/foundation/ecs/schema/cmake/engine_ecs_codegen.cmake)
engine_target_add_ecs_component_codegen(
    NAME visual_component_codegen
    TARGET ecs_visual
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/VisualSchema.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/Visual.hpp
    LOGICAL_PATH lux/engine/ecs/Visual.hpp
    SYMBOL visual
)

engine_target_add_ecs_component_codegen(
    NAME visibility_component_codegen
    TARGET ecs_visual
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/VisualSchema.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/VisibilityComponent.hpp
    LOGICAL_PATH lux/engine/ecs/VisibilityComponent.hpp
    SYMBOL visibility
)

engine_target_add_ecs_component_codegen(
    NAME renderable_mesh_component_codegen
    TARGET ecs_visual
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/VisualSchema.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/RenderableMeshComponent.hpp
    LOGICAL_PATH lux/engine/ecs/RenderableMeshComponent.hpp
    SYMBOL renderableMesh
)
