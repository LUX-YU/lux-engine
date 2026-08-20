add_executable(asset_vfs_test asset_vfs_test.cpp)
target_link_libraries(
    asset_vfs_test
    PRIVATE
        lux::engine::resource::asset
        lux::engine::authoring::authoring_assets
)

add_executable(asset_pak_test asset_pak_test.cpp)
target_link_libraries(
    asset_pak_test
    PRIVATE
        lux::engine::resource::asset
        lux::engine::authoring::authoring_assets
        lux::engine::toolchain::toolchain_asset_cook
)
target_include_directories(
    asset_pak_test
    PRIVATE ${PROJECT_SOURCE_DIR}/modules/resource/asset/pinclude
)

add_executable(flow_graph_codec_test flow_graph_codec_test.cpp)
target_link_libraries(
    flow_graph_codec_test
    PRIVATE
        lux::engine::authoring::authoring_assets
)
if(TARGET ecs_meta)
    target_link_libraries(flow_graph_codec_test PRIVATE ecs_meta)
endif()
target_include_directories(
    flow_graph_codec_test
    PRIVATE ${PROJECT_SOURCE_DIR}/engine/authoring/assets/pinclude
)

add_executable(
    texture_importer_compression_test
    texture_importer_compression_test.cpp
)
target_link_libraries(
    texture_importer_compression_test
    PRIVATE
        lux::engine::toolchain::toolchain_texture_importer
)

foreach(
    target
    asset_vfs_test
    asset_pak_test
    flow_graph_codec_test
    texture_importer_compression_test
)
    lux_classify_target(
        TARGET  ${target}
        LAYER   TOOLCHAIN
        PRODUCT TEST
        ROLE    INTEGRATION
    )
endforeach()
