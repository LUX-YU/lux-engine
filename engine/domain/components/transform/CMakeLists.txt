find_package(Eigen3 CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_TRANSFORM_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_TRANSFORM_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_TRANSFORM_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/transform/visibility.h
)

add_component(
    COMPONENT_NAME ecs_transform
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_transform
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TransformSchema.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TransformEvaluation.cpp
)

component_include_directories(
    ecs_transform
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    ecs_transform
    PRIVATE LUX_ENGINE_ECS_TRANSFORM_LIBRARY
)
target_link_libraries(
    ecs_transform
    PUBLIC
        lux::engine::ecs::ecs_core
        lux::engine::ecs::ecs_schema
        lux::engine::ecs::ecs_snapshot
        lux::engine::ecs::ecs_hierarchy
        lux::engine::core::math
        lux::engine::core::serialization
)
component_add_internal_dependencies(ecs_transform ecs_core ecs_schema ecs_snapshot ecs_hierarchy)
component_add_transitive_commands(
    ecs_transform
    "find_package(Eigen3 CONFIG REQUIRED)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math serialization)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_core ecs_schema ecs_snapshot ecs_hierarchy)"
)

include_component_cmake_scripts(meta)
include(${PROJECT_SOURCE_DIR}/engine/foundation/ecs/schema/cmake/engine_ecs_codegen.cmake)
engine_target_add_ecs_component_codegen(
    NAME transform_component_codegen
    TARGET ecs_transform
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/TransformSchema.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/ecs/Transform.hpp
    LOGICAL_PATH lux/engine/ecs/Transform.hpp
    SYMBOL transform
)
