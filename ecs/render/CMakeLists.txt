# ECS rendering is split along two independent axes:
#   * render_components[_2d|_3d] own data/schema;
#   * render_core/render_2d/render_3d/render_pixel_integration own behavior.
#
# The dimensional component and extraction targets are intentionally separate.
# A headless consumer can link the component target without pulling a render
# session, while a 2D pack has no target or schema edge to the 3D domain.

set(LUX_ECS_RENDER_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

function(lux_configure_render_interface target)
    lux_classify_target(
        TARGET  ${target}
        LAYER   ECS
        PRODUCT RUNTIME
        ROLE    DOMAIN
    )
    component_include_directories(
        ${target}
        BUILD_TIME_EXPORT
            ${LUX_ECS_RENDER_INCLUDE_DIR}
            ${LUX_GENERATE_HEADER_DIR}
        INSTALL_TIME include
    )
endfunction()

# ---------------------------------------------------------------------------
# Component/schema ownership. These targets emit no runtime library.
# ---------------------------------------------------------------------------
add_interface_component(
    COMPONENT_NAME render_components
    NAMESPACE      lux::engine::ecs
)
lux_configure_render_interface(render_components)
target_link_libraries(
    render_components
    INTERFACE
        lux::engine::ecs::core
        lux::engine::function::render_client
        lux::engine::resource::asset_core
        lux::engine::resource::description
        lux::engine::core::math
        lux::engine::core::meta
)
component_add_transitive_commands(
    render_components
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core)"
    "find_package(lux-engine-function REQUIRED COMPONENTS render_client)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset_core description)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math meta)"
)

if(NOT LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
    add_interface_component(
        COMPONENT_NAME render_components_2d
        NAMESPACE      lux::engine::ecs
    )
    lux_configure_render_interface(render_components_2d)
    target_link_libraries(
        render_components_2d
        INTERFACE
            lux::engine::ecs::render_components
    )
    component_add_transitive_commands(
        render_components_2d
        "find_package(lux-engine-ecs REQUIRED COMPONENTS render_components)"
    )
endif()

add_interface_component(
    COMPONENT_NAME render_components_3d
    NAMESPACE      lux::engine::ecs
)
lux_configure_render_interface(render_components_3d)
target_link_libraries(
    render_components_3d
    INTERFACE
        lux::engine::ecs::render_components
        lux::engine::function::animation
        lux::engine::resource::entity_scene
)
component_add_transitive_commands(
    render_components_3d
    "find_package(lux-engine-ecs REQUIRED COMPONENTS render_components)"
    "find_package(lux-engine-function REQUIRED COMPONENTS animation)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS entity_scene)"
)

# ---------------------------------------------------------------------------
# Dimension-neutral extraction graph and view/resource lifecycle.
# ---------------------------------------------------------------------------
if(NOT LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
add_component(
    COMPONENT_NAME render_core
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_render_core
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SceneRenderBinding.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderBridgeDiagnostics.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderSystemBuilder.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/RenderSystem.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CameraViewSubsystem.cpp
)
lux_classify_target(
    TARGET  render_core
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)
component_include_directories(
    render_core
    BUILD_TIME_EXPORT
        ${LUX_ECS_RENDER_INCLUDE_DIR}
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(render_core PRIVATE LUX_FUNCTION_LIBRARY)
target_link_libraries(
    render_core
    PUBLIC
        lux::engine::ecs::render_components
        lux::engine::function::render_client
        lux::engine::resource::asset_core
)
component_add_transitive_commands(
    render_core
    "find_package(lux-engine-ecs REQUIRED COMPONENTS render_components)"
    "find_package(lux-engine-function REQUIRED COMPONENTS render_client)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset_core)"
)

# 2D extraction is header-only. Sparse Pixel and Tilemap runtimes are excluded
# into explicit leaves, so an image-only application acquires neither domain.
add_interface_component(
    COMPONENT_NAME render_2d
    NAMESPACE      lux::engine::ecs
)
lux_configure_render_interface(render_2d)
target_link_libraries(
    render_2d
    INTERFACE
        lux::engine::ecs::render_core
        lux::engine::ecs::render_components_2d
)
component_add_transitive_commands(
    render_2d
    "find_package(lux-engine-ecs REQUIRED COMPONENTS render_core render_components_2d)"
)

# 3D extraction is header-only. World streaming belongs to Runtime World
# Partition and never scans renderable ECS pools.
add_interface_component(
    COMPONENT_NAME render_3d
    NAMESPACE      lux::engine::ecs
)
lux_configure_render_interface(render_3d)
target_link_libraries(
    render_3d
    INTERFACE
        lux::engine::ecs::render_core
        lux::engine::ecs::render_components_3d
)
component_add_transitive_commands(
    render_3d
    "find_package(lux-engine-ecs REQUIRED COMPONENTS render_core render_components_3d)"
)

add_component(
    COMPONENT_NAME render_pixel_integration
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_render_pixel_integration
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/PixelField2DSubsystem.cpp
)
lux_classify_target(
    TARGET  render_pixel_integration
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)
component_include_directories(
    render_pixel_integration
    BUILD_TIME_EXPORT
        ${LUX_ECS_RENDER_INCLUDE_DIR}
        ${LUX_GENERATE_HEADER_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)
target_compile_definitions(
    render_pixel_integration
    PRIVATE LUX_FUNCTION_LIBRARY
)
target_link_libraries(
    render_pixel_integration
    PUBLIC
        lux::engine::ecs::render_2d
        lux::engine::ecs::pixel
)
component_add_transitive_commands(
    render_pixel_integration
    "find_package(lux-engine-ecs REQUIRED COMPONENTS render_2d pixel)"
)

add_component(
    COMPONENT_NAME render_tilemap_integration
    NAMESPACE      lux::engine::ecs
    OUTPUT_NAME    lux_engine_ecs_render_tilemap_integration
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Tilemap2DSubsystem.cpp
)
lux_classify_target(
    TARGET  render_tilemap_integration
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)
component_include_directories(
    render_tilemap_integration
    BUILD_TIME_EXPORT
        ${LUX_ECS_RENDER_INCLUDE_DIR}
        ${LUX_GENERATE_HEADER_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)
target_compile_definitions(
    render_tilemap_integration
    PRIVATE LUX_FUNCTION_LIBRARY
)
target_link_libraries(
    render_tilemap_integration
    PUBLIC
        lux::engine::ecs::render_2d
        lux::engine::ecs::tilemap
)
component_add_transitive_commands(
    render_tilemap_integration
    "find_package(lux-engine-ecs REQUIRED COMPONENTS render_2d tilemap)"
)
endif()

# ---------------------------------------------------------------------------
# Reflection follows component ownership, not extraction behavior or packs.
# ---------------------------------------------------------------------------
if(LUX_BUILD_COMPONENT_META)
    include_component_cmake_scripts(meta)

    function(lux_add_render_meta_family slug component_target)
        set(headers ${ARGN})

        add_library(${slug}_meta SHARED
            ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
        lux_classify_target(
            TARGET  ${slug}_meta
            LAYER   ECS
            PRODUCT RUNTIME
            ROLE    INTEGRATION
        )
        set_target_properties(${slug}_meta PROPERTIES
            OUTPUT_NAME lux_engine_ecs_${slug}_meta)
        target_link_libraries(
            ${slug}_meta
            PRIVATE ${component_target} lux::engine::core::meta
        )
        target_compile_definitions(${slug}_meta PRIVATE LUX_FUNCTION_LIBRARY)

        set(_lua_meta_args "")
        if(LUX_BUILD_COMPONENT_LUA_META)
            add_library(${slug}_lua_meta SHARED
                ${CMAKE_CURRENT_SOURCE_DIR}/src/meta_sidecar_anchor.cpp)
            lux_classify_target(
                TARGET  ${slug}_lua_meta
                LAYER   ECS
                PRODUCT RUNTIME
                ROLE    INTEGRATION
            )
            set_target_properties(${slug}_lua_meta PROPERTIES
                OUTPUT_NAME lux_engine_ecs_${slug}_lua_meta)
            target_link_libraries(
                ${slug}_lua_meta
                PRIVATE
                    ${component_target}
                    lux::engine::core::meta
                    lux::engine::function::script_lua
            )
            target_compile_definitions(
                ${slug}_lua_meta PRIVATE LUX_FUNCTION_LIBRARY)
            list(APPEND _lua_meta_args
                LUA_SIDECAR_TARGET ${slug}_lua_meta
                LUA_BINDING)
        endif()

        engine_enable_module_meta(
            TARGET             ${slug}
            SIDECAR_TARGET     ${slug}_meta
            TARGET_FILES       ${headers}
            ${_lua_meta_args}
        )
    endfunction()

    lux_add_render_meta_family(
        render_components
        render_components
        ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/PrimaryCameraTag.hpp
        ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/VisualTransitionComponent.hpp
    )
    if(NOT LUX_BUILD_PROFILE STREQUAL "TOOLCHAIN")
        lux_add_render_meta_family(
            render_components_2d
            render_components_2d
            ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/2d/Image2DComponent.hpp
            ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/2d/Camera2DComponent.hpp
            ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/2d/Grid2DComponent.hpp
            ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/2d/Order2DComponents.hpp
        )
    endif()
    lux_add_render_meta_family(
        render_components_3d
        render_components_3d
        ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/3d/MeshComponent.hpp
        ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/3d/ClassicMeshBatchComponent.hpp
        ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/3d/VisualLodNodeComponent.hpp
        ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/3d/SkeletalMeshComponent.hpp
        ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/3d/AnimatorComponent.hpp
        ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/3d/Camera3DComponent.hpp
        ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/3d/SkyboxComponent.hpp
        ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/3d/HeightFogComponent.hpp
        ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/3d/Grid3DComponent.hpp
        ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/3d/DirectionalLightComponent.hpp
        ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/3d/PointLightComponent.hpp
        ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/3d/SpotLightComponent.hpp
        ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/3d/WaterSurfaceComponent.hpp
        ${LUX_ECS_RENDER_INCLUDE_DIR}/lux/engine/ecs/render/components/3d/SceneSettingsComponent.hpp
    )
endif()
