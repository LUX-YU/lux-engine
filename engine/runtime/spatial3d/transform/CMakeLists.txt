generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_SPATIAL3D_TRANSFORM_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_SPATIAL3D_TRANSFORM_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_SPATIAL3D_TRANSFORM_DLL_DISABLE
    GENERATE_FILE_PATH
        lux/engine/runtime/spatial3d/transform/visibility.h
)

add_component(
    COMPONENT_NAME runtime_pack_spatial3d_transform
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_pack_spatial3d_transform
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Spatial3DTransformContribution.cpp
)

component_include_directories(
    runtime_pack_spatial3d_transform
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(
    runtime_pack_spatial3d_transform
    PRIVATE LUX_ENGINE_RUNTIME_SPATIAL3D_TRANSFORM_LIBRARY
)

target_link_libraries(
    runtime_pack_spatial3d_transform
    PUBLIC
        lux::engine::runtime::runtime_scene_contributions
        lux::engine::ecs::core
        lux::engine::ecs::transform
)

component_add_transitive_commands(
    runtime_pack_spatial3d_transform
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core transform)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_scene_contributions)"
)

lux_classify_target(
    TARGET  runtime_pack_spatial3d_transform
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)
