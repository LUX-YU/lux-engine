generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_TILEMAP_PREPARE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_TILEMAP_PREPARE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_TILEMAP_PREPARE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/assets/tilemap/visibility.h
)

add_component(
    COMPONENT_NAME runtime_tilemap_prepare
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_tilemap_prepare
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TilemapPrepareService.cpp
)

component_include_directories(
    runtime_tilemap_prepare
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    runtime_tilemap_prepare
    PRIVATE LUX_ENGINE_RUNTIME_TILEMAP_PREPARE_LIBRARY
)

if(MSVC)
    target_compile_options(
        runtime_tilemap_prepare
        PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor
    )
endif()

target_link_libraries(
    runtime_tilemap_prepare
    PUBLIC
        lux::engine::runtime::runtime_execution
        lux::engine::ecs::tilemap_streaming
)

component_add_transitive_commands(
    runtime_tilemap_prepare
    "find_package(lux-engine-ecs REQUIRED COMPONENTS tilemap_streaming)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_execution)"
)

lux_classify_target(
    TARGET  runtime_tilemap_prepare
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_subdirectory(test)
