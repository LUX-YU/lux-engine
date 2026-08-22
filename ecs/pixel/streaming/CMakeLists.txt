find_package(lux-cxx REQUIRED COMPONENTS memory)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_PIXEL_STREAMING_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_PIXEL_STREAMING_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_PIXEL_STREAMING_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/pixel/streaming/visibility.h
)

add_component(
    COMPONENT_NAME pixel_streaming
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_pixel_streaming
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Infinite2DPixelSystem.cpp
)

component_include_directories(
    pixel_streaming
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(
    pixel_streaming
    PRIVATE LUX_ENGINE_ECS_PIXEL_STREAMING_LIBRARY
)

target_link_libraries(
    pixel_streaming
    PUBLIC
        lux::cxx::memory
        lux::engine::core::async_port
        lux::engine::ecs::core
        lux::engine::ecs::entity_scene
        lux::engine::ecs::pixel
        lux::engine::ecs::spatial2d_streaming
)

component_add_transitive_commands(
    pixel_streaming
    "find_package(lux-cxx REQUIRED COMPONENTS memory)"
    "find_package(lux-engine-core REQUIRED COMPONENTS async_port)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core entity_scene pixel spatial2d_streaming)"
)

lux_classify_target(
    TARGET  pixel_streaming
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)
