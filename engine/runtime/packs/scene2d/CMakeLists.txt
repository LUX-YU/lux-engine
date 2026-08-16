# Orthogonal 2D Scene contributions. Physics and transform/simulation remain
# headless; presentation alone links Pixel/Tilemap/Render integration leaves.

function(lux_add_spatial2d_pack TARGET_NAME OUTPUT_NAME SOURCE VISIBILITY)
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
    ENABLE_MACRO_NAME       LUX_ENGINE_PACK_SPATIAL2D_TRANSFORM_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_PACK_SPATIAL2D_TRANSFORM_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_PACK_SPATIAL2D_TRANSFORM_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/packs/scene2d/transform_visibility.h
)
lux_add_spatial2d_pack(
    runtime_pack_spatial2d_transform
    lux_engine_runtime_pack_spatial2d_transform
    Transform2DContribution.cpp
    LUX_ENGINE_PACK_SPATIAL2D_TRANSFORM_LIBRARY
)
target_link_libraries(
    runtime_pack_spatial2d_transform
    PUBLIC
        lux::engine::runtime::runtime_scene_contributions
        lux::engine::ecs::core
        lux::engine::ecs::transform
)
component_add_transitive_commands(
    runtime_pack_spatial2d_transform
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core transform)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_scene_contributions)"
)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_PACK_SIMULATION2D_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_PACK_SIMULATION2D_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_PACK_SIMULATION2D_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/packs/scene2d/simulation_visibility.h
)
lux_add_spatial2d_pack(
    runtime_pack_simulation2d
    lux_engine_runtime_pack_simulation2d
    Simulation2DContribution.cpp
    LUX_ENGINE_PACK_SIMULATION2D_LIBRARY
)
target_link_libraries(
    runtime_pack_simulation2d
    PUBLIC
        lux::engine::runtime::runtime_scene_contributions
        lux::engine::ecs::core
        lux::engine::ecs::physics
)
component_add_transitive_commands(
    runtime_pack_simulation2d
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core physics)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_scene_contributions)"
)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_PACK_PHYSICS2D_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_PACK_PHYSICS2D_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_PACK_PHYSICS2D_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/packs/scene2d/physics_visibility.h
)
lux_add_spatial2d_pack(
    runtime_pack_physics2d
    lux_engine_runtime_pack_physics2d
    Physics2DContribution.cpp
    LUX_ENGINE_PACK_PHYSICS2D_LIBRARY
)
target_link_libraries(
    runtime_pack_physics2d
    PUBLIC
        lux::engine::runtime::runtime_scene_contributions
        lux::engine::runtime::runtime_pack_simulation2d
        lux::engine::runtime::runtime_pack_spatial2d_transform
        lux::engine::ecs::core
        lux::engine::ecs::physics
        lux::engine::ecs::physics2d
)
component_add_transitive_commands(
    runtime_pack_physics2d
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core physics physics2d)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_pack_simulation2d runtime_pack_spatial2d_transform runtime_scene_contributions)"
)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_PACK_TILEMAP2D_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_PACK_TILEMAP2D_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_PACK_TILEMAP2D_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/packs/scene2d/tilemap_visibility.h
)
lux_add_spatial2d_pack(
    runtime_pack_tilemap2d
    lux_engine_runtime_pack_tilemap2d
    Tilemap2DContribution.cpp
    LUX_ENGINE_PACK_TILEMAP2D_LIBRARY
)
target_link_libraries(
    runtime_pack_tilemap2d
    PUBLIC
        lux::engine::runtime::runtime_scene_contributions
        lux::engine::runtime::runtime_scene_core
        lux::engine::runtime::runtime_spatial2d_tilemap
        lux::engine::ecs::core
        lux::engine::ecs::tilemap
)
component_add_transitive_commands(
    runtime_pack_tilemap2d
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core tilemap)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_scene_contributions runtime_scene_core runtime_spatial2d_tilemap)"
)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_PACK_PRESENTATION2D_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_PACK_PRESENTATION2D_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_PACK_PRESENTATION2D_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/runtime/packs/scene2d/presentation_visibility.h
)
lux_add_spatial2d_pack(
    runtime_pack_presentation2d
    lux_engine_runtime_pack_presentation2d
    Presentation2DContribution.cpp
    LUX_ENGINE_PACK_PRESENTATION2D_LIBRARY
)
if(MSVC)
    target_compile_options(
        runtime_pack_presentation2d
        PRIVATE "$<$<CONFIG:Debug>:/bigobj>"
    )
endif()
target_link_libraries(
    runtime_pack_presentation2d
    PUBLIC
        lux::engine::runtime::runtime_scene_contributions
        lux::engine::runtime::runtime_pack_simulation2d
        lux::engine::runtime::runtime_pack_spatial2d_transform
        lux::engine::runtime::runtime_assets
        lux::engine::ecs::core
        lux::engine::ecs::pixel
        lux::engine::ecs::tilemap
        lux::engine::ecs::render_2d
        lux::engine::ecs::render_pixel_integration
        lux::engine::ecs::render_tilemap_integration
        lux::engine::ecs::animation_2d
        lux::engine::ecs::physics
        lux::engine::ecs::physics2d_pixel_integration
        lux::engine::resource::asset_core
)
component_add_transitive_commands(
    runtime_pack_presentation2d
    "find_package(lux-engine-ecs REQUIRED COMPONENTS animation_2d core physics physics2d_pixel_integration pixel render_2d render_pixel_integration render_tilemap_integration tilemap)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset_core)"
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_assets runtime_pack_simulation2d runtime_pack_spatial2d_transform runtime_scene_contributions)"
)
