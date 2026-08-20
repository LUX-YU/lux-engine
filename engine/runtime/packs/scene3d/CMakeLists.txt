# Orthogonal 3D Scene contributions. Selecting Physics or Navigation does not
# link Render; selecting Presentation does not install either simulation leaf.

function(lux_add_spatial3d_pack TARGET_NAME OUTPUT_NAME SOURCE VISIBILITY)
    add_component(
        COMPONENT_NAME ${TARGET_NAME}
        NAMESPACE      lux::engine::runtime
        OUTPUT_NAME    ${OUTPUT_NAME}
        SOURCE_FILES   ${CMAKE_CURRENT_SOURCE_DIR}/src/${SOURCE}
    )
    lux_classify_target(
        TARGET  ${TARGET_NAME}
        LAYER   RUNTIME
        PRODUCT RUNTIME
        ROLE    INTEGRATION
    )
    component_include_directories(
        ${TARGET_NAME}
        BUILD_TIME_EXPORT
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${LUX_GENERATE_HEADER_DIR}
        INSTALL_TIME include
    )
    target_compile_definitions(${TARGET_NAME} PRIVATE ${VISIBILITY})
endfunction()

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_PACK_ANIMATION3D_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_PACK_ANIMATION3D_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_PACK_ANIMATION3D_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/packs/scene3d/animation_visibility.h
)
lux_add_spatial3d_pack(
    runtime_pack_animation3d
    lux_engine_runtime_pack_animation3d
    Animation3DContribution.cpp
    LUX_ENGINE_PACK_ANIMATION3D_LIBRARY
)
target_link_libraries(
    runtime_pack_animation3d
    PUBLIC
        lux::engine::runtime::runtime_scene_contributions
        lux::engine::ecs::core
        lux::engine::ecs::animation_3d
        lux::engine::runtime::runtime_assets
        lux::engine::runtime::runtime_pack_spatial3d_transform
        lux::engine::resource::asset
)
component_add_transitive_commands(
    runtime_pack_animation3d
    "find_package(lux-engine-ecs REQUIRED COMPONENTS animation_3d core)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_assets runtime_pack_spatial3d_transform runtime_scene_contributions)"
)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_PACK_PHYSICS3D_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_PACK_PHYSICS3D_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_PACK_PHYSICS3D_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/packs/scene3d/physics_visibility.h
)
lux_add_spatial3d_pack(
    runtime_pack_physics3d
    lux_engine_runtime_pack_physics3d
    Physics3DContribution.cpp
    LUX_ENGINE_PACK_PHYSICS3D_LIBRARY
)
target_link_libraries(
    runtime_pack_physics3d
    PUBLIC
        lux::engine::runtime::runtime_scene_contributions
        lux::engine::runtime::runtime_scene_core
        lux::engine::runtime::runtime_entity_scene
        lux::engine::runtime::runtime_pack_spatial3d_transform
        lux::engine::runtime::runtime_spatial3d_physics
        lux::engine::ecs::core
        lux::engine::ecs::physics3d
)
component_add_transitive_commands(
    runtime_pack_physics3d
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core physics3d)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_entity_scene runtime_scene_contributions runtime_scene_core runtime_pack_spatial3d_transform runtime_spatial3d_physics)"
)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_PACK_NAVIGATION3D_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_PACK_NAVIGATION3D_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_PACK_NAVIGATION3D_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/packs/scene3d/navigation_visibility.h
)
lux_add_spatial3d_pack(
    runtime_pack_navigation3d
    lux_engine_runtime_pack_navigation3d
    Navigation3DContribution.cpp
    LUX_ENGINE_PACK_NAVIGATION3D_LIBRARY
)
target_link_libraries(
    runtime_pack_navigation3d
    PUBLIC
        lux::engine::runtime::runtime_scene_contributions
        lux::engine::runtime::runtime_scene_core
        lux::engine::runtime::runtime_entity_scene
        lux::engine::runtime::runtime_spatial3d_navigation_adapter
        lux::engine::ecs::core
        lux::engine::ecs::navigation3d
        lux::engine::function::navigation_detour3d
)
component_add_transitive_commands(
    runtime_pack_navigation3d
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core navigation3d)"
    "find_package(lux-engine-function REQUIRED COMPONENTS navigation_detour3d)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_entity_scene runtime_scene_contributions runtime_scene_core runtime_spatial3d_navigation_adapter)"
)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_PACK_PRESENTATION3D_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_PACK_PRESENTATION3D_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_PACK_PRESENTATION3D_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/packs/scene3d/presentation_visibility.h
)
lux_add_spatial3d_pack(
    runtime_pack_presentation3d
    lux_engine_runtime_pack_presentation3d
    Presentation3DContribution.cpp
    LUX_ENGINE_PACK_PRESENTATION3D_LIBRARY
)
if(MSVC)
    target_compile_options(
        runtime_pack_presentation3d
        PRIVATE "$<$<CONFIG:Debug>:/bigobj>"
    )
endif()
target_link_libraries(
    runtime_pack_presentation3d
    PUBLIC
        lux::engine::runtime::runtime_scene_contributions
        lux::engine::runtime::runtime_scene_core
        lux::engine::runtime::runtime_assets
        lux::engine::runtime::runtime_entity_scene
        lux::engine::runtime::runtime_render_scene
        lux::engine::runtime::runtime_pack_spatial3d_transform
        lux::engine::ecs::core
        lux::engine::ecs::render_3d
        lux::engine::resource::asset
)
component_add_transitive_commands(
    runtime_pack_presentation3d
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core render_3d)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_assets runtime_entity_scene runtime_render_scene runtime_scene_contributions runtime_scene_core runtime_pack_spatial3d_transform)"
)
