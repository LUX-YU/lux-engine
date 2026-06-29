## engine/asset_pipeline — build-time asset packing tool.
##
## lux_asset_packer links ONLY lux::engine::resource::asset (no imgui / no MLIR).
## It is a build-time dependency of the RUNTIME itself:
##   - modules/function/render/cmake/embed_builtin_shaders.cmake invokes it to
##     embed builtin shader .luxasset files into the render library;
##   - modules/resource/asset/cmake/add_asset.cmake's add_asset() drives it.
## Therefore engine/CMakeLists.txt adds this subdirectory UNCONDITIONALLY,
## OUTSIDE the LUX_BUILD_EDITOR gate.
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
    lux::engine::resource::asset
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
)
