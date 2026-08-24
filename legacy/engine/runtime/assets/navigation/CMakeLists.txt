generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_ASSETS_NAVIGATION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_ASSETS_NAVIGATION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_ASSETS_NAVIGATION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/assets/navigation/visibility.h
)

add_component(
    COMPONENT_NAME runtime_navigation3d_prepare
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_navigation3d_prepare
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Navigation3DPrepareService.cpp
)

component_include_directories(
    runtime_navigation3d_prepare
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    runtime_navigation3d_prepare
    PRIVATE LUX_ENGINE_RUNTIME_ASSETS_NAVIGATION_LIBRARY
)

if(MSVC)
    target_compile_options(
        runtime_navigation3d_prepare
        PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor
    )
endif()

target_link_libraries(
    runtime_navigation3d_prepare
    PUBLIC
        lux::engine::runtime::runtime_execution
        lux::engine::ecs::navigation3d_streaming
    PRIVATE
        lux::engine::function::navigation_detour3d
)

component_add_transitive_commands(
    runtime_navigation3d_prepare
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_execution)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS navigation3d_streaming)"
    "find_package(lux-engine-function REQUIRED COMPONENTS navigation_detour3d)"
)

lux_classify_target(
    TARGET  runtime_navigation3d_prepare
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    spatial3d_navigation_adapter_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/spatial3d_navigation_adapter_test.cpp
)
target_link_libraries(
    spatial3d_navigation_adapter_test
    PRIVATE
        lux::engine::runtime::runtime_navigation3d_prepare
        lux::engine::runtime::runtime_entity_scene
        lux::engine::ecs::navigation3d_streaming
)
if(MSVC)
    target_compile_options(spatial3d_navigation_adapter_test PRIVATE /UNDEBUG)
else()
    target_compile_options(spatial3d_navigation_adapter_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  spatial3d_navigation_adapter_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    INTEGRATION
)
