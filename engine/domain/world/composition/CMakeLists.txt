generate_visibility_header(
    ENABLE_MACRO_NAME LUX_ENGINE_WORLD_COMPOSITION_LIBRARY
    PUBLIC_MACRO_NAME LUX_ENGINE_WORLD_COMPOSITION_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_ENGINE_WORLD_COMPOSITION_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/world/composition/visibility.h
)

add_component(
    COMPONENT_NAME world_composition
    NAMESPACE lux::engine::world
    OUTPUT_NAME lux_engine_world_composition
    SOURCE_FILES src/World.cpp src/WorldSchema.cpp src/WorldSpatialIndex.cpp
)

component_include_directories(
    world_composition
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

target_compile_definitions(world_composition PRIVATE LUX_ENGINE_WORLD_COMPOSITION_LIBRARY)

target_link_libraries(
    world_composition
    PUBLIC
        lux::engine::world::description
        lux::engine::ecs::ecs_core
        lux::engine::ecs::ecs_schema
        lux::engine::core::error
        lux::engine::spatial::spatial3d_index
)

component_add_transitive_commands(
    world_composition
    "find_package(lux-engine-world-description REQUIRED COMPONENTS world_description)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_core ecs_schema)"
    "find_package(lux-engine-core REQUIRED COMPONENTS error)"
    "find_package(lux-engine-domain-spatial3d REQUIRED COMPONENTS spatial3d_index)"
)

include_component_cmake_scripts(meta)
include(${PROJECT_SOURCE_DIR}/engine/foundation/ecs/schema/cmake/engine_ecs_codegen.cmake)

# Identity stays a foundation value; this domain projects its fields without a reverse target dependency.
engine_target_add_static_type_info(
    NAME world_identity_codegen
    TARGET world_composition
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldSchema.cpp
    HEADERS ${PROJECT_SOURCE_DIR}/engine/foundation/identity/world/include/lux/engine/world/WorldObjectId.hpp
    LOGICAL_PATHS lux/engine/world/WorldObjectId.hpp
    OUT_DIR ${LUX_GENERATE_HEADER_DIR}
)

engine_target_add_ecs_component_codegen(
    NAME world_observer_component_codegen
    TARGET world_composition
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldSchema.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/world/ObserverComponent.hpp
    LOGICAL_PATH lux/engine/world/ObserverComponent.hpp
    SYMBOL observer
)

engine_target_add_ecs_component_codegen(
    NAME world_lifecycle_component_codegen
    TARGET world_composition
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/WorldSchema.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/world/LifecyclePolicyComponent.hpp
    LOGICAL_PATH lux/engine/world/LifecyclePolicyComponent.hpp
    SYMBOL lifecyclePolicy
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-world-composition
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::world
    COMPONENTS world_composition
)
