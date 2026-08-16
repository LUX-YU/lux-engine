## engine/toolchain/asset/packer — build-time asset packing tools.
##
## lux_asset_packer links ONLY lux::engine::resource::asset_codecs (no imgui / no MLIR).
## It is a build-time dependency of the RUNTIME itself:
##   - modules/function/render/features/cmake/embed_builtin_shaders.cmake invokes it to
##     embed builtin shader .luxasset files into the render library;
##   - modules/resource/asset/cmake/add_asset.cmake's add_asset() drives it.
## Therefore the repository root configures this subdirectory before Function
## when compiling for the host. This is a build-order edge, never a Runtime
## link edge.
##
## This is a leaf executable: nothing links against it, it exports no headers,
## so it is a plain add_executable (not an add_component).

add_executable(
    lux_asset_packer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AssetTool.cpp
)

target_link_libraries(
    lux_asset_packer
    PRIVATE
        lux::engine::resource::asset_codecs
        lux::engine::toolchain::toolchain_asset_cook
        lux::engine::toolchain::toolchain_shader_asset
        lux::engine::toolchain::toolchain_texture_importer
)

lux_classify_target(
    TARGET  lux_asset_packer
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    COMPOSITION
)

if(WIN32)
    add_custom_command(TARGET lux_asset_packer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:lux_asset_packer>
                $<TARGET_FILE_DIR:lux_asset_packer>
        COMMAND_EXPAND_LISTS
        COMMENT "Copy runtime DLLs for lux_asset_packer"
    )
endif()

install(
    TARGETS lux_asset_packer
    RUNTIME DESTINATION bin
    COMPONENT lux_toolchain
)

## lux_shader_emitter — a thin CLI shell wrapping .lglsl → canonical GLSL.
## All synthesis logic lives in the Toolchain-owned canonical emitter; this
## executable only handles argv parsing and file IO.
## Like lux_asset_packer, it is a build-time tool: shader build manifests run
## it before glslc to expand .lglsl into full GLSL; after install, external
## projects invoke it via find_program.
add_executable(
    lux_shader_emitter
    ${CMAKE_CURRENT_SOURCE_DIR}/src/ShaderEmitterTool.cpp
)

lux_classify_target(
    TARGET lux_shader_emitter
    LAYER TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE COMPOSITION
)

target_link_libraries(
    lux_shader_emitter
    PRIVATE
    lux::engine::toolchain::toolchain_shader_emitter
)

install(
    TARGETS lux_shader_emitter
    RUNTIME DESTINATION bin
    COMPONENT lux_toolchain
)
