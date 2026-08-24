generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_PIXEL_ASSETS_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_PIXEL_ASSETS_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_PIXEL_ASSETS_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/assets/pixel/visibility.h
)

add_component(
    COMPONENT_NAME runtime_pixel_assets
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_pixel_assets
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Infinite2DPixelContent.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Infinite2DPixelPrepareService.cpp
)

component_include_directories(
    runtime_pixel_assets
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    runtime_pixel_assets
    PRIVATE LUX_ENGINE_RUNTIME_PIXEL_ASSETS_LIBRARY
)

if(MSVC)
    target_compile_options(
        runtime_pixel_assets
        PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor
    )
endif()

target_link_libraries(
    runtime_pixel_assets
    PUBLIC
        lux::engine::runtime::runtime_entity_scene
        lux::engine::runtime::runtime_execution
        lux::engine::ecs::pixel_streaming
        lux::engine::ecs::scene_format
    PRIVATE
        lux::cxx::algorithm
        lux::engine::core::serialization
        lux::engine::ecs::component_archive
)

component_add_transitive_commands(
    runtime_pixel_assets
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_entity_scene runtime_execution)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS component_archive pixel_streaming scene_format)"
)

lux_classify_target(
    TARGET  runtime_pixel_assets
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    infinite2d_headless_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/infinite2d_headless_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/infinite2d_headless_test.cpp
)
target_link_libraries(
    infinite2d_headless_test
    PRIVATE
        lux::engine::runtime::runtime_pixel_assets
        lux::engine::runtime::runtime_execution
        lux::engine::runtime::runtime_tilemap_prepare
        lux::engine::ecs::pixel_streaming
        lux::engine::ecs::tilemap_streaming
        lux::engine::core::serialization
        lux::engine::ecs::component_archive
        lux::engine::ecs::tilemap
        lux::engine::resource::asset
)
if(MSVC)
    target_compile_options(infinite2d_headless_test PRIVATE /UNDEBUG)
else()
    target_compile_options(infinite2d_headless_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  infinite2d_headless_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    INTEGRATION
)
