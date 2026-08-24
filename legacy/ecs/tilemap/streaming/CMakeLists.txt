find_package(lux-cxx REQUIRED COMPONENTS memory)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_TILEMAP_STREAMING_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_TILEMAP_STREAMING_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_TILEMAP_STREAMING_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/tilemap/streaming/visibility.h
)

add_component(
    COMPONENT_NAME tilemap_streaming
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_tilemap_streaming
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/TilemapChunkSystem.cpp
)

component_include_directories(
    tilemap_streaming
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(
    tilemap_streaming
    PRIVATE LUX_ENGINE_ECS_TILEMAP_STREAMING_LIBRARY
)

target_link_libraries(
    tilemap_streaming
    PUBLIC
        lux::cxx::memory
        lux::engine::core::async_port
        lux::engine::ecs::core
        lux::engine::ecs::entity_scene
        lux::engine::ecs::tilemap
)

component_add_transitive_commands(
    tilemap_streaming
    "find_package(lux-cxx REQUIRED COMPONENTS memory)"
    "find_package(lux-engine-core REQUIRED COMPONENTS async_port)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core entity_scene tilemap)"
)

lux_classify_target(
    TARGET  tilemap_streaming
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)
