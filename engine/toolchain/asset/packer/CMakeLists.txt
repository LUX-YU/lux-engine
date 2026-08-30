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
endif()

if(BUILD_TESTING)
    set(packer_shader_source
        ${CMAKE_BINARY_DIR}/engine/toolchain/asset/shader/minimal.vert.spv
    )
    set(packer_smoke_root ${CMAKE_CURRENT_BINARY_DIR}/smoke)
    add_test(
        NAME toolchain_asset_packer_cli_test
        COMMAND ${CMAKE_COMMAND}
            -DPACKER=$<TARGET_FILE:lux_asset_packer>
            -DTEXTURE_SOURCE=${CMAKE_SOURCE_DIR}/modules/function/render/features/assets/textures/ringed_gas_giant_planet.png
            -DSHADER_SOURCE=${packer_shader_source}
            -DOUTPUT_ROOT=${packer_smoke_root}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/test/PackerSmoke.cmake
    )
    set_tests_properties(toolchain_asset_packer_cli_test PROPERTIES TIMEOUT 30)

    set(packer_embed_asset ${CMAKE_CURRENT_BINARY_DIR}/embed_shader.luxasset)
    set(packer_embed_header ${CMAKE_CURRENT_BINARY_DIR}/embed_shader.hpp)
    add_custom_command(
        OUTPUT ${packer_embed_asset} ${packer_embed_header}
        COMMAND $<TARGET_FILE:lux_asset_packer>
            --type shader
            --source_path ${packer_shader_source}
            --target_path ${packer_embed_asset}
        COMMAND $<TARGET_FILE:lux_asset_packer>
            --embed
            --target_path ${packer_embed_asset}
            --source_path ${packer_embed_header}
            --embed_name smoke_shader
            --embed_namespace lux::toolchain::smoke
        DEPENDS lux_asset_packer ${packer_shader_source}
        VERBATIM
    )
    add_executable(
        toolchain_asset_packer_embed_test
        ${CMAKE_CURRENT_SOURCE_DIR}/test/EmbedSmoke.cpp
        ${packer_embed_header}
    )
    target_include_directories(
        toolchain_asset_packer_embed_test
        PRIVATE ${CMAKE_CURRENT_BINARY_DIR}
    )
    lux_add_build_tool_dependency(
        CONSUMER toolchain_asset_packer_embed_test
        TOOL lux_asset_packer
    )
    lux_classify_target(
        TARGET  toolchain_asset_packer_embed_test
        LAYER   TOOLCHAIN
        PRODUCT TEST
        ROLE    COMPOSITION
    )
    add_test(NAME toolchain_asset_packer_embed_test COMMAND toolchain_asset_packer_embed_test)
endif()
