include_component_cmake_scripts(meta)

add_component(
    COMPONENT_NAME scene_toolchain
    NAMESPACE lux::engine::toolchain
    STATIC
    SOURCE_FILES src/SceneStreamingBuild.cpp src/MeshSceneContent.cpp
)

component_include_directories(
    scene_toolchain
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_link_libraries(
    scene_toolchain
    PUBLIC
        lux::engine::engine_metadata
        lux::engine::assets::engine_assets
        lux::engine::scene::scene_asset
        lux::engine::world::world_storage
    PRIVATE
        lux::engine::engine_context
        lux::engine::ecs::ecs_visual
        lux::engine::ecs::ecs_transform
)

lux_add_codegen_job(
    NAME scene_grid_codec
    GENERATOR ${LUX_META_GENERATOR}
    MARKER luxref
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneStreamingBuild.cpp
    TARGET_FILES ${PROJECT_SOURCE_DIR}/engine/domain/world/description/include/lux/engine/world/SceneGridConfiguration.hpp
    LOGICAL_PATHS lux/engine/world/SceneGridConfiguration.hpp
)

lux_codegen_add_projection(
    JOB scene_grid_codec
    NAME scene_codec
    TEMPLATE ${CMAKE_CURRENT_SOURCE_DIR}/template/scene_grid_codec.template
    OUTPUT_ROOT ${LUX_GENERATE_HEADER_DIR}
    OUTPUT_SUFFIX .scene_codec.hpp
)

lux_target_add_codegen(TARGET scene_toolchain JOB scene_grid_codec DONT_ADD_TO_SOURCE)

component_add_transitive_commands(
    scene_toolchain
    "find_package(lux-engine-runtime-metadata REQUIRED COMPONENTS engine_metadata)"
    "find_package(lux-engine-assets REQUIRED COMPONENTS engine_assets)"
    "find_package(lux-engine-scene-asset REQUIRED COMPONENTS scene_asset)"
    "find_package(lux-engine-world-storage REQUIRED COMPONENTS world_storage)"
    "find_package(lux-engine-context REQUIRED COMPONENTS engine_context)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_visual ecs_transform)"
)

lux_classify_target(TARGET scene_toolchain LAYER ENGINE PRODUCT TOOLCHAIN ROLE LIBRARY)

lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-toolchain
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::toolchain
    COMPONENTS scene_toolchain
)
