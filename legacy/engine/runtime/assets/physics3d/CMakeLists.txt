find_package(stduuid CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_ASSETS_PHYSICS3D_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_ASSETS_PHYSICS3D_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_ASSETS_PHYSICS3D_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/assets/physics3d/visibility.h
)

add_component(
    COMPONENT_NAME runtime_physics3d_prepare
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_physics3d_prepare
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/StaticCollider3DPrepareService.cpp
)

component_include_directories(
    runtime_physics3d_prepare
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    runtime_physics3d_prepare
    PRIVATE LUX_ENGINE_RUNTIME_ASSETS_PHYSICS3D_LIBRARY
)

if(MSVC)
    target_compile_options(
        runtime_physics3d_prepare
        PRIVATE /permissive- /Zc:__cplusplus /Zc:preprocessor
    )
endif()

target_link_libraries(
    runtime_physics3d_prepare
    PUBLIC
        lux::engine::runtime::runtime_execution
        lux::engine::ecs::physics3d_streaming
    PRIVATE
        lux::engine::ecs::physics3d
)

component_add_transitive_commands(
    runtime_physics3d_prepare
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_execution)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS physics3d physics3d_streaming)"
)

lux_classify_target(
    TARGET  runtime_physics3d_prepare
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    DOMAIN
)
