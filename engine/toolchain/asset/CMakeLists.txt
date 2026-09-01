set(LUX_SHADER_COOKER_TEST_SPIRV
    "${CMAKE_CURRENT_BINARY_DIR}/shader/minimal.vert.spv"
)
add_subdirectory(shader)
add_subdirectory(texture)
add_subdirectory(model)
add_subdirectory(packer)

if(LUX_PROFILE_HAS_TOOLCHAIN)
    install_components(
        PROJECT_NAME lux-engine-toolchain-asset
        VERSION      ${PROJECT_VERSION}
        NAMESPACE    lux::engine::toolchain
        COMPONENTS
            toolchain_shader_asset_cooker
            toolchain_texture_cooker
            toolchain_model_cooker
    )
endif()
