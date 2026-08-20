set(_asset_root ${CMAKE_CURRENT_LIST_DIR}/..)

find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time)

add_component(
    COMPONENT_NAME asset_codecs
    NAMESPACE      lux::engine::resource
    OUTPUT_NAME    lux_engine_asset_codecs
    SOURCE_FILES
        src/AssetSerDeserFactory.cpp
        src/TextureCodec.cpp
        src/MaterialSerDeser.cpp
        src/MaterialInstanceSerDeser.cpp
        src/MeshSerDeser.cpp
        src/MeshDescriptionCodec.cpp
        src/ModelCodec.cpp
        src/ShaderSerDeser.cpp
        src/ScriptSerDeser.cpp
        src/ScriptDescriptionCodec.cpp
        src/SkeletonSerDeser.cpp
        src/SkeletonDescriptionCodec.cpp
        src/AnimationClipSerDeser.cpp
        src/AnimationClipDescriptionCodec.cpp
        src/TextureAtlasAssets.cpp
        src/TerrainTileCodec.cpp
        src/TilemapChunkCodec.cpp
        src/StaticColliderBatch3DCodec.cpp
)
component_include_directories(
    asset_codecs
    BUILD_TIME_EXPORT
        ${_asset_root}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${_asset_root}/pinclude
)
target_compile_definitions(asset_codecs PRIVATE LUX_ASSET_CODECS_LIBRARY)
target_link_libraries(
    asset_codecs
    PUBLIC
        lux::cxx::compile_time
        lux::engine::resource::asset_core
        lux::engine::resource::description
    PRIVATE
        lux::engine::core::serialization
        lux::engine::platform::common
)
component_add_internal_dependencies(asset_codecs description)
component_add_cmake_scripts(
    asset_codecs
    ${_asset_root}/cmake/add_asset.cmake
)
component_add_transitive_commands(
    asset_codecs
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset_identity asset_core description)"
)
lux_classify_target(
    TARGET  asset_codecs
    LAYER   RESOURCE
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

foreach(_codec_test IN ITEMS
    terrain_tile_codec_test
    tilemap_chunk_codec_test
    static_collider_batch3d_codec_test
)
    add_executable(
        ${_codec_test}
        ${CMAKE_CURRENT_SOURCE_DIR}/test/${_codec_test}.cpp
    )
    target_link_libraries(
        ${_codec_test}
        PRIVATE
            lux::cxx::algorithm
            lux::engine::resource::asset_codecs
    )
    if(MSVC)
        target_compile_options(${_codec_test} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${_codec_test} PRIVATE -UNDEBUG)
    endif()
    lux_classify_target(
        TARGET  ${_codec_test}
        LAYER   RESOURCE
        PRODUCT TEST
        ROLE    DOMAIN
    )
endforeach()

add_executable(
    asset_content_codecs_public_link_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/asset_content_codecs_public_link_test.cpp
)
target_link_libraries(
    asset_content_codecs_public_link_test
    PRIVATE lux::engine::resource::asset_codecs
)
lux_classify_target(
    TARGET  asset_content_codecs_public_link_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    DOMAIN
)
