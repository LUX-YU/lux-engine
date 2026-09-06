add_executable(
    lux_asset_packer
    ${CMAKE_CURRENT_SOURCE_DIR}/src/AssetPacker.cpp
)

target_link_libraries(
    lux_asset_packer
    PRIVATE
        lux::engine::resource::asset
        lux::engine::toolchain::toolchain_shader_asset_cooker
        lux::engine::toolchain::toolchain_texture_cooker
        lux::engine::toolchain::toolchain_model_cooker
)

lux_classify_target(
    TARGET  lux_asset_packer
    LAYER   TOOLCHAIN
    PRODUCT BUILD_TOOL
    ROLE    COMPOSITION
)

if(WIN32)
    add_custom_command(
        TARGET lux_asset_packer
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_RUNTIME_DLLS:lux_asset_packer>
            $<TARGET_FILE_DIR:lux_asset_packer>
        COMMAND_EXPAND_LISTS
        VERBATIM
    )
endif()

if(LUX_PROFILE_HAS_TOOLCHAIN)
    install(
        TARGETS lux_asset_packer
        RUNTIME DESTINATION bin
        COMPONENT lux_toolchain
    )
    install(
        FILES $<TARGET_RUNTIME_DLLS:lux_asset_packer>
        DESTINATION bin
        COMPONENT lux_toolchain
    )
endif()
