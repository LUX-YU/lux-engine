generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_SPATIAL2D_TILEMAP_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_SPATIAL2D_TILEMAP_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_SPATIAL2D_TILEMAP_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/spatial2d/tilemap/visibility.h
)

add_component(
    COMPONENT_NAME runtime_spatial2d_tilemap
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_spatial2d_tilemap
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TilemapPrepareService.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TilemapChunkSystem.cpp
)

lux_classify_target(
    TARGET  runtime_spatial2d_tilemap
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

component_include_directories(
    runtime_spatial2d_tilemap
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_include_directories(
    runtime_spatial2d_tilemap
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
)

target_compile_definitions(
    runtime_spatial2d_tilemap
    PRIVATE LUX_ENGINE_RUNTIME_SPATIAL2D_TILEMAP_LIBRARY
)

target_link_libraries(
    runtime_spatial2d_tilemap
    PUBLIC
        lux::engine::runtime::runtime_entity_scene
        lux::engine::runtime::runtime_execution
        lux::engine::ecs::tilemap
        lux::engine::resource::asset_codecs
)

component_add_transitive_commands(
    runtime_spatial2d_tilemap
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_entity_scene runtime_execution)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS tilemap)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset_codecs)"
)

add_subdirectory(test)
