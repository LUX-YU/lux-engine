generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_INFINITE2D_PIXEL_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_INFINITE2D_PIXEL_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_INFINITE2D_PIXEL_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/spatial2d/infinite/pixel_visibility.h
)

add_component(
    COMPONENT_NAME runtime_infinite2d_systems
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_infinite2d_systems
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Infinite2DPixelContent.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Infinite2DPixelPrepareService.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Infinite2DPixelSystem.cpp
)

component_include_directories(
    runtime_infinite2d_systems
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_include_directories(
    runtime_infinite2d_systems
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
)

target_compile_definitions(
    runtime_infinite2d_systems
    PRIVATE LUX_ENGINE_RUNTIME_INFINITE2D_PIXEL_LIBRARY
)

target_link_libraries(
    runtime_infinite2d_systems
    PUBLIC
        lux::engine::runtime::runtime_entity_scene
        lux::engine::runtime::runtime_execution
        lux::engine::ecs::pixel
        lux::engine::ecs::scene_format
        lux::engine::ecs::spatial2d_streaming
    PRIVATE
        lux::cxx::algorithm
        lux::engine::core::serialization
        lux::engine::ecs::component_archive
)

component_add_transitive_commands(
    runtime_infinite2d_systems
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_entity_scene runtime_execution)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS pixel scene_format component_archive spatial2d_streaming)"
)

lux_classify_target(
    TARGET  runtime_infinite2d_systems
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

add_executable(
    infinite2d_headless_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/infinite2d_headless_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/test/infinite2d_headless_test.cpp
)
target_link_libraries(
    infinite2d_headless_test
    PRIVATE
        lux::engine::runtime::runtime_infinite2d_systems
        lux::engine::runtime::runtime_execution
        lux::engine::runtime::runtime_spatial2d_tilemap
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
