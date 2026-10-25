add_component(
    COMPONENT_NAME process_render_upload
    NAMESPACE lux::engine::process
    STATIC
    SOURCE_FILES src/RasterAssetPreparation.cpp
)

component_include_directories(
    process_render_upload
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)

target_link_libraries(
    process_render_upload
    PUBLIC lux::engine::process::asset_loading lux::engine::function::render
    PRIVATE lux::engine::core::error
)

component_add_transitive_commands(
    process_render_upload
    "find_package(lux-engine-process-asset-loading REQUIRED COMPONENTS process_asset_loading)"
    "find_package(lux-engine-render REQUIRED COMPONENTS render)"
)

lux_classify_target(TARGET process_render_upload LAYER PROCESS PRODUCT RUNTIME ROLE LIBRARY)

lux_engine_install_components(
    PROJECT_NAME lux-engine-process-render-upload
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::process
    COMPONENTS process_render_upload
)
