if(NOT TARGET toolchain_shader)
    add_subdirectory(shader)
endif()
add_subdirectory(asset)
add_subdirectory(flowforge)
add_subdirectory(entity_scene)
add_subdirectory(spatial3d_scene)
add_subdirectory(game_export)

set(LUX_TOOLCHAIN_COMPONENTS
    toolchain_shader
    toolchain_shader_ir
    toolchain_shader_emitter
    toolchain_shader_asset
    toolchain_asset_cook
    toolchain_texture_importer
    toolchain_material_compiler
    toolchain_builtin_content
    toolchain_model_importer
    toolchain_entity_scene_cook
    toolchain_spatial3d_scene_cook
    toolchain_game_export
)

if(TARGET toolchain_shader_compiler)
    list(APPEND LUX_TOOLCHAIN_COMPONENTS toolchain_shader_compiler)
endif()

if(TARGET toolchain_flowforge)
    list(APPEND LUX_TOOLCHAIN_COMPONENTS toolchain_flowforge)
endif()

install_components(
    PROJECT_NAME lux-engine-toolchain
    VERSION      0.0.1
    NAMESPACE    lux::engine::toolchain
    COMPONENTS ${LUX_TOOLCHAIN_COMPONENTS}
)
