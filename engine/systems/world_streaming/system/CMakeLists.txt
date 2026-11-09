generate_visibility_header(
    ENABLE_MACRO_NAME LUX_ENGINE_SCENE_WORLD_LOADING_LIBRARY
    PUBLIC_MACRO_NAME LUX_ENGINE_SCENE_WORLD_LOADING_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_ENGINE_SCENE_WORLD_LOADING_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/scene/world_loading/visibility.h
)

add_component(
    COMPONENT_NAME system_world_streaming
    NAMESPACE lux::engine::scene
    OUTPUT_NAME lux_engine_system_world_streaming
    STATIC
    SOURCE_FILES
        src/WorldLoadingSystem.cpp
)

component_include_directories(
    system_world_streaming
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_include_directories(system_world_streaming PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)

target_compile_definitions(system_world_streaming PUBLIC LUX_ENGINE_SCENE_WORLD_LOADING_DLL_DISABLE)
target_link_libraries(
    system_world_streaming PUBLIC
    lux::engine::scene::scene_composition
    lux::engine::world::world_storage
    lux::engine::assets::engine_assets
    lux::engine::engine_context
    lux::engine::scene::scene_asset
    lux::engine::ecs::ecs_transform
)

component_add_transitive_commands(
    system_world_streaming
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-scene-asset REQUIRED COMPONENTS scene_asset)"
    "find_package(lux-engine-world-storage REQUIRED COMPONENTS world_storage)"
    "find_package(lux-engine-assets REQUIRED COMPONENTS engine_assets)"
    "find_package(lux-engine-context REQUIRED COMPONENTS engine_context)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_transform)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-world-loading
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::scene
    COMPONENTS system_world_streaming
)

if(BUILD_TESTING)
    add_executable(system_world_streaming_test test/world_loading.cpp)

    target_link_libraries(
        system_world_streaming_test PRIVATE
        lux::engine::scene::system_world_streaming
        lux::engine::ecs::ecs_hierarchy
        lux::engine::ecs::ecs_transform
        lux::engine::engine_initialization
        lux::engine::toolchain::scene_toolchain
    )

    if(MSVC)
        target_compile_options(system_world_streaming_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(system_world_streaming_test PRIVATE -UNDEBUG)
    endif()

    add_test(NAME scene.world_loading COMMAND system_world_streaming_test ${CMAKE_CURRENT_BINARY_DIR}/fixture)

    set_tests_properties(scene.world_loading PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
endif()

lux_classify_target(TARGET system_world_streaming LAYER SYSTEMS PRODUCT RUNTIME ROLE LIBRARY)
