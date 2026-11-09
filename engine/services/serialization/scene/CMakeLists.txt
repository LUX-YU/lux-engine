generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SCENE_ASSET_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SCENE_ASSET_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SCENE_ASSET_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/services/serialization/scene/visibility.h
)

add_component(
    COMPONENT_NAME scene_asset
    NAMESPACE      lux::engine::scene
    OUTPUT_NAME    lux_engine_scene_asset
    SOURCE_FILES
        src/SceneAssetCodec.cpp
        src/WorldMaterializer.cpp
        src/SceneContent.cpp
        src/ScenePersistence.cpp
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
        lux::engine::assets::engine_assets
        lux::engine::ecs::ecs_schema
        lux::engine::world::world_composition
        lux::engine::core::serialization
        lux::engine::platform::platform_file
    PRIVATE
        lux::engine::core::error
        lux::engine::world::world_storage
)

component_add_transitive_commands(
    scene_asset
    "find_package(lux-engine-scene-description REQUIRED COMPONENTS scene_description)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-assets REQUIRED COMPONENTS engine_assets)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_schema)"
    "find_package(lux-engine-world-composition REQUIRED COMPONENTS world_composition)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization error)"
    "find_package(lux-engine-platform REQUIRED COMPONENTS platform_file)"
    "find_package(lux-engine-world-storage REQUIRED COMPONENTS world_storage)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-asset
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_asset
)


if(BUILD_TESTING)
    add_executable(world_partition_decoder_test test/partition_decoder.cpp)

    target_link_libraries(
        world_partition_decoder_test PRIVATE
        scene_asset
        lux::engine::toolchain::scene_toolchain
    )

    if(MSVC)
        target_compile_options(world_partition_decoder_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(world_partition_decoder_test PRIVATE -UNDEBUG)
    endif()

    add_test(
        NAME world.partition_decoder_parity
        COMMAND world_partition_decoder_test "${CMAKE_CURRENT_BINARY_DIR}/file-ranges"
    )

    set_tests_properties(world.partition_decoder_parity PROPERTIES TIMEOUT 30 LABELS "native;lifecycle")

    add_executable(scene_file_test test/SceneAssetFile.cpp)

    target_link_libraries(
        scene_file_test PRIVATE
        scene_asset
        lux::engine::engine_initialization
        lux::engine::engine_runtime
        lux::engine::toolchain::scene_toolchain
        lux::engine::scene::system_world_streaming
        lux::engine::ecs::ecs_hierarchy
    )

    if(LUX_ENGINE_WITH_RENDER)
        target_link_libraries(
            scene_file_test PRIVATE
            lux::engine::systems::system_render
            lux::engine::ecs::ecs_visual
        )
    endif()

    if(MSVC)
        target_compile_options(scene_file_test PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(scene_file_test PRIVATE -UNDEBUG)
    endif()

    add_test(NAME framework.scene_file COMMAND scene_file_test "${CMAKE_CURRENT_BINARY_DIR}/scene-file-data")
    set_tests_properties(framework.scene_file PROPERTIES TIMEOUT 60)
endif()
