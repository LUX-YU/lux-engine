add_component(
    COMPONENT_NAME scene_render_builtin
    NAMESPACE lux::engine::scene
    STATIC
    SOURCE_FILES src/RasterAssetPipelines.cpp
)

component_include_directories(
    scene_render_builtin
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
    INSTALL_TIME include
)

target_link_libraries(
    scene_render_builtin
    PUBLIC lux::engine::engine_render_context lux::engine::process::process_render_upload
)

lux_classify_target(TARGET scene_render_builtin LAYER SCENE PRODUCT RUNTIME ROLE LIBRARY)

component_add_transitive_commands(
    scene_render_builtin
    "find_package(lux-engine-context-render REQUIRED COMPONENTS engine_render_context)"
    "find_package(lux-engine-process-render-upload REQUIRED COMPONENTS process_render_upload)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-render-builtin
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::scene
    COMPONENTS scene_render_builtin
)
