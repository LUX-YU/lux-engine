generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SCENE_ASSET_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SCENE_ASSET_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SCENE_ASSET_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/scene/asset/visibility.h
)
add_component(
    COMPONENT_NAME scene_asset
    NAMESPACE      lux::engine::scene
    OUTPUT_NAME    lux_engine_scene_asset
    SOURCE_FILES src/SceneAssetCodec.cpp src/ScenePackage.cpp src/SceneCapture.cpp src/SceneConstruction.cpp
)
component_include_directories(
    scene_asset
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(scene_asset PRIVATE LUX_ENGINE_SCENE_ASSET_LIBRARY)
target_link_libraries(
    scene_asset
    PUBLIC
        lux::engine::scene::scene_description
        lux::engine::resource::asset
        lux::engine::world::world_asset lux::engine::simulation::simulation_asset
        lux::engine::simulation::ecs::schema lux::engine::process::world_loading
    PUBLIC lux::engine::core::serialization
)
component_add_transitive_commands(
    scene_asset
    "find_package(lux-engine-scene-description REQUIRED COMPONENTS scene_description)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-world-asset REQUIRED COMPONENTS world_asset)"
    "find_package(lux-engine-simulation-asset REQUIRED COMPONENTS simulation_asset)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS schema)"
    "find_package(lux-engine-process-world-loading REQUIRED COMPONENTS process_world_loading)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-asset
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_asset
)
