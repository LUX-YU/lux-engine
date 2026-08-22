find_package(lux-cxx REQUIRED COMPONENTS compile_time core)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ECS_SPATIAL2D_STREAMING_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ECS_SPATIAL2D_STREAMING_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ECS_SPATIAL2D_STREAMING_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/ecs/spatial2d/streaming/visibility.h
)

add_component(
    COMPONENT_NAME spatial2d_streaming
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_spatial2d_streaming
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Spatial2DSectionIndex.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SpatialInterest2DSystem.cpp
)

component_include_directories(
    spatial2d_streaming
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    spatial2d_streaming
    PRIVATE LUX_ECS_SPATIAL2D_STREAMING_LIBRARY
)

target_link_libraries(
    spatial2d_streaming
    PUBLIC
        lux::cxx::compile_time
        lux::cxx::core
        lux::engine::core::math
        lux::engine::ecs::core
        lux::engine::ecs::entity_scene
        lux::engine::ecs::scene_format
        lux::engine::ecs::spatial2d_components
        lux::engine::ecs::transform
)

component_add_transitive_commands(
    spatial2d_streaming
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time core)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core entity_scene scene_format spatial2d_components transform)"
)

lux_classify_target(
    TARGET  spatial2d_streaming
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    spatial2d_interest_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/spatial2d_interest_test.cpp
)
target_link_libraries(
    spatial2d_interest_test
    PRIVATE lux::engine::ecs::spatial2d_streaming
)
if(MSVC)
    target_compile_options(spatial2d_interest_test PRIVATE /UNDEBUG)
else()
    target_compile_options(spatial2d_interest_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  spatial2d_interest_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    DOMAIN
)
