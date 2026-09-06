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
    SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneAssetCodec.cpp
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
    PRIVATE lux::engine::core::serialization
)
component_add_transitive_commands(
    scene_asset
    "find_package(lux-engine-scene-description REQUIRED COMPONENTS scene_description)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
)
lux_classify_target(
    TARGET  scene_asset
    LAYER   SCENE
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

install_components(
    PROJECT_NAME lux-engine-scene-asset
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_asset
)
