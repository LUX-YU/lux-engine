# Product-level direct system composition. These are ordinary functions, not
# Runtime objects or registries; domain Systems remain owned by ECS targets.
generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_TILEMAP_SYSTEMS_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_TILEMAP_SYSTEMS_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_TILEMAP_SYSTEMS_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/scene/composition/tilemap_visibility.h)

add_component(
    COMPONENT_NAME runtime_scene_tilemap_composition
    NAMESPACE      lux::engine::runtime
    OUTPUT_NAME    lux_engine_runtime_scene_tilemap_composition
    SOURCE_FILES   ${CMAKE_CURRENT_SOURCE_DIR}/src/InstallTilemapSystems.cpp)
component_include_directories(
    runtime_scene_tilemap_composition
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include)
target_compile_definitions(
    runtime_scene_tilemap_composition
    PRIVATE LUX_ENGINE_TILEMAP_SYSTEMS_LIBRARY)
target_link_libraries(
    runtime_scene_tilemap_composition
    PUBLIC
        lux::engine::runtime::runtime_scene_core
        lux::engine::ecs::core
        lux::engine::ecs::tilemap
        lux::engine::ecs::tilemap_streaming)
component_add_transitive_commands(
    runtime_scene_tilemap_composition
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core tilemap tilemap_streaming)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_scene_core)")
lux_classify_target(
    TARGET runtime_scene_tilemap_composition
    LAYER RUNTIME
    PRODUCT RUNTIME
    ROLE INTEGRATION)

function(lux_add_scene_composition TARGET_NAME OUTPUT_NAME SOURCE VISIBILITY)
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
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_SPATIAL3D_SYSTEMS_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_SPATIAL3D_SYSTEMS_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_SPATIAL3D_SYSTEMS_DLL_DISABLE
    GENERATE_FILE_PATH
        lux/engine/runtime/scene/composition/spatial3d_visibility.h
)
lux_add_scene_composition(
    runtime_scene_spatial3d_composition
    lux_engine_runtime_scene_spatial3d_composition
    InstallSpatial3DSystems.cpp
    LUX_ENGINE_RUNTIME_SPATIAL3D_SYSTEMS_LIBRARY
)
target_link_libraries(
    runtime_scene_spatial3d_composition
    PUBLIC
        lux::engine::runtime::runtime_entity_scene
        lux::engine::ecs::core
        lux::engine::ecs::entity_scene
        lux::engine::ecs::spatial3d_scene_format
        lux::engine::ecs::spatial3d_streaming
)
component_add_transitive_commands(
    runtime_scene_spatial3d_composition
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core entity_scene spatial3d_scene_format spatial3d_streaming)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_entity_scene)"
)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_PHYSICS3D_SYSTEMS_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_PHYSICS3D_SYSTEMS_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_PHYSICS3D_SYSTEMS_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/scene/composition/physics3d_visibility.h
)
lux_add_scene_composition(
    runtime_scene_physics3d_composition
    lux_engine_runtime_scene_physics3d_composition
    InstallPhysics3DSystems.cpp
    LUX_ENGINE_RUNTIME_PHYSICS3D_SYSTEMS_LIBRARY
)
target_link_libraries(
    runtime_scene_physics3d_composition
    PUBLIC
        lux::engine::runtime::runtime_scene_core
        lux::engine::runtime::runtime_entity_scene
        lux::engine::ecs::transform
        lux::engine::ecs::core
        lux::engine::ecs::physics3d
        lux::engine::ecs::physics3d_streaming
)
component_add_transitive_commands(
    runtime_scene_physics3d_composition
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core physics3d physics3d_streaming)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_entity_scene runtime_scene_core)"
)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_NAVIGATION3D_SYSTEMS_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_NAVIGATION3D_SYSTEMS_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_NAVIGATION3D_SYSTEMS_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/scene/composition/navigation3d_visibility.h
)
lux_add_scene_composition(
    runtime_scene_navigation3d_composition
    lux_engine_runtime_scene_navigation3d_composition
    InstallNavigation3DSystems.cpp
    LUX_ENGINE_RUNTIME_NAVIGATION3D_SYSTEMS_LIBRARY
)
target_link_libraries(
    runtime_scene_navigation3d_composition
    PUBLIC
        lux::engine::runtime::runtime_scene_core
        lux::engine::runtime::runtime_entity_scene
        lux::engine::ecs::core
        lux::engine::ecs::navigation3d
        lux::engine::ecs::navigation3d_streaming
        lux::engine::function::navigation_detour3d
)
component_add_transitive_commands(
    runtime_scene_navigation3d_composition
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core navigation3d navigation3d_streaming)"
    "find_package(lux-engine-function REQUIRED COMPONENTS navigation_detour3d)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_entity_scene runtime_scene_core)"
)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_RUNTIME_PRESENTATION3D_SYSTEMS_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_RUNTIME_PRESENTATION3D_SYSTEMS_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_RUNTIME_PRESENTATION3D_SYSTEMS_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/scene/composition/presentation3d_visibility.h
)
lux_add_scene_composition(
    runtime_scene_presentation3d_composition
    lux_engine_runtime_scene_presentation3d_composition
    InstallPresentation3DSystems.cpp
    LUX_ENGINE_RUNTIME_PRESENTATION3D_SYSTEMS_LIBRARY
)
if(MSVC)
    target_compile_options(
        runtime_scene_presentation3d_composition
        PRIVATE "$<$<CONFIG:Debug>:/bigobj>"
    )
endif()
target_link_libraries(
    runtime_scene_presentation3d_composition
    PUBLIC
        lux::engine::runtime::runtime_scene_core
        lux::engine::runtime::runtime_assets
        lux::engine::runtime::runtime_entity_scene
        lux::engine::runtime::runtime_render_scene
        lux::engine::ecs::transform
        lux::engine::ecs::core
        lux::engine::ecs::render_3d
        lux::engine::ecs::render_content_3d_integration
        lux::engine::resource::asset
)
component_add_transitive_commands(
    runtime_scene_presentation3d_composition
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core render_3d render_content_3d_integration)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_assets runtime_entity_scene runtime_render_scene runtime_scene_core)"
)
