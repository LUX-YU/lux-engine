find_package(recastnavigation 1.6.0 CONFIG REQUIRED)
find_package(meshoptimizer CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_SPATIAL3D_SCENE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_TOOLCHAIN_SPATIAL3D_SCENE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_TOOLCHAIN_SPATIAL3D_SCENE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/toolchain/spatial3d_scene/visibility.h
)

add_component(
    COMPONENT_NAME toolchain_spatial3d_scene_cook
    STATIC
    NAMESPACE      lux::engine::toolchain
    OUTPUT_NAME    lux_engine_toolchain_spatial3d_scene_cook
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Spatial3DAuthoringSource.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Spatial3DEntitySceneAdapter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Spatial3DNavigationCook.cpp
)

lux_classify_target(
    TARGET  toolchain_spatial3d_scene_cook
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    DOMAIN
)

component_include_directories(
    toolchain_spatial3d_scene_cook
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)

target_compile_definitions(
    toolchain_spatial3d_scene_cook
    PUBLIC LUX_ENGINE_TOOLCHAIN_SPATIAL3D_SCENE_DLL_DISABLE
)

target_link_libraries(
    toolchain_spatial3d_scene_cook
    PUBLIC
        lux::engine::toolchain::toolchain_entity_scene_cook
        lux::engine::scene::scene_package
        lux::engine::ecs::core
        lux::engine::ecs::identity
        lux::engine::resource::asset_identity
        lux::engine::resource::spatial
        lux::engine::resource::spatial3d_scene
    PRIVATE
        lux::engine::authoring::authoring_world
        lux::engine::ecs::render_components_3d
        lux::engine::ecs::spatial3d_components
        lux::engine::ecs::terrain_components
        lux::engine::ecs::physics3d
        lux::engine::ecs::navigation3d
        lux::engine::function::navigation_detour3d
        lux::engine::resource::classic_mesh_content
        lux::engine::resource::asset_codecs
        lux::engine::resource::terrain_content
        lux::engine::resource::physics3d_content
        lux::engine::core::serialization
        lux::cxx::algorithm
        RecastNavigation::Recast
        meshoptimizer::meshoptimizer
)

component_add_transitive_commands(
    toolchain_spatial3d_scene_cook
    "find_package(recastnavigation 1.6.0 CONFIG REQUIRED)"
    "find_package(meshoptimizer CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm)"
    "find_package(lux-engine-authoring REQUIRED COMPONENTS authoring_world)"
    "find_package(lux-engine-toolchain REQUIRED COMPONENTS toolchain_entity_scene_cook)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_package)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core identity render_components_3d spatial3d_components terrain_components physics3d navigation3d)"
    "find_package(lux-engine-function REQUIRED COMPONENTS navigation_detour3d)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset_identity asset_codecs spatial classic_mesh_content spatial3d_scene terrain_content physics3d_content)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
)

if(TARGET ecs_meta AND
   TARGET render_components_meta AND
   TARGET render_components_3d_meta AND
   TARGET spatial3d_components_meta AND
   TARGET terrain_components_meta AND
   TARGET physics3d_meta AND
   TARGET navigation3d_meta)
    add_executable(
        spatial3d_entity_scene_adapter_test
        ${CMAKE_CURRENT_SOURCE_DIR}/test/spatial3d_entity_scene_adapter_test.cpp
    )
    target_link_libraries(
        spatial3d_entity_scene_adapter_test
        PRIVATE
            lux::engine::toolchain::toolchain_spatial3d_scene_cook
            lux::engine::ecs::render_components_3d
            lux::engine::ecs::spatial3d_components
            lux::engine::ecs::terrain_components
            lux::engine::ecs::physics3d
            lux::engine::ecs::navigation3d
            lux::engine::resource::classic_mesh_content
            lux::engine::resource::terrain_content
            lux::engine::resource::physics3d_content
            ecs_meta
            render_components_meta
            render_components_3d_meta
            spatial3d_components_meta
            terrain_components_meta
            physics3d_meta
            navigation3d_meta
            lux::engine::core::meta
    )
    if(MSVC)
        target_compile_options(
            spatial3d_entity_scene_adapter_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(
            spatial3d_entity_scene_adapter_test PRIVATE -UNDEBUG)
    endif()
    lux_classify_target(
        TARGET  spatial3d_entity_scene_adapter_test
        LAYER   TOOLCHAIN
        PRODUCT TEST
        ROLE    DOMAIN
    )
endif()
