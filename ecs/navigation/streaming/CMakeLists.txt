generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_ECS_NAVIGATION_STREAMING_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_ECS_NAVIGATION_STREAMING_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_ECS_NAVIGATION_STREAMING_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/navigation/streaming/visibility.h
)

add_component(
    COMPONENT_NAME navigation3d_streaming
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_navigation3d_streaming
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Spatial3DNavigationAdapterSystem.cpp
)

component_include_directories(
    navigation3d_streaming
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    navigation3d_streaming
    PRIVATE LUX_ENGINE_ECS_NAVIGATION_STREAMING_LIBRARY
)

target_link_libraries(
    navigation3d_streaming
    PUBLIC
        lux::engine::core::async_port
        lux::engine::ecs::core
        lux::engine::ecs::entity_scene
        lux::engine::ecs::navigation3d
        lux::engine::ecs::scene_format
        lux::engine::function::navigation_detour3d
)

component_add_transitive_commands(
    navigation3d_streaming
    "find_package(lux-engine-core REQUIRED COMPONENTS async_port)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core entity_scene navigation3d scene_format)"
    "find_package(lux-engine-function REQUIRED COMPONENTS navigation_detour3d)"
)

lux_classify_target(
    TARGET  navigation3d_streaming
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)
