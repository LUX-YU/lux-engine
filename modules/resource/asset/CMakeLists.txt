find_package(stduuid CONFIG REQUIRED)
find_package(
    lux-cxx
    CONFIG
    REQUIRED
    COMPONENTS
        algorithm
        compile_time
        core
        memory
        reflection_runtime
)

include_component_cmake_scripts(lux::cxx::reflection_runtime)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ASSET_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ASSET_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ASSET_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/resource/asset/visibility.h
)

add_component(
    COMPONENT_NAME asset
    NAMESPACE      lux::engine::resource
    OUTPUT_NAME    lux_engine_asset
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/AnimationClipAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/AssetCodecCatalog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/AssetManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/AssetSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/AssetVfs.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/MaterialAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/MaterialInstanceAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/MeshAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/ModelAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/ScriptAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/ShaderAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/SkeletonAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/TextureAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/TextureAtlasAssetValues.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/VirtualPath.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/AnimationClipDescriptionCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/AnimationClipSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/AssetSerDeserFactory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/MaterialInstanceSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/MaterialSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/MeshDescriptionCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/MeshSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/ModelCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/ScriptDescriptionCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/ScriptSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/ShaderSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/SkeletonDescriptionCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/SkeletonSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/StaticColliderBatch3DCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/TerrainTileCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/TextureAtlasAssets.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/TextureCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/codecs/TilemapChunkCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/pak/PakAssetProvider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/pak/PakCodec.cpp
)

component_include_directories(
    asset
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

target_compile_definitions(asset PRIVATE LUX_ASSET_LIBRARY)

target_link_libraries(
    asset
    PUBLIC
        stduuid
        lux::cxx::compile_time
        lux::cxx::memory
        lux::engine::core::math
        lux::engine::core::meta
        lux::engine::core::serialization
        lux::engine::resource::description
    PRIVATE
        lux::cxx::algorithm
        lux::cxx::core
        lux::cxx::reflection_runtime
)

component_add_internal_dependencies(asset description)
component_add_cmake_scripts(
    asset
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/add_asset.cmake
)
component_add_transitive_commands(
    asset
    "find_package(stduuid CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time core memory reflection_runtime)"
    "find_package(lux-engine-core REQUIRED COMPONENTS math meta serialization)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS description)"
)

lux_classify_target(
    TARGET  asset
    LAYER   RESOURCE
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    asset_codec_catalog_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/asset_codec_catalog_test.cpp
)
target_link_libraries(
    asset_codec_catalog_test
    PRIVATE lux::engine::resource::asset
)
lux_classify_target(
    TARGET  asset_codec_catalog_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    DOMAIN
)

foreach(_codec_test IN ITEMS
    terrain_tile_codec_test
    tilemap_chunk_codec_test
    static_collider_batch3d_codec_test
)
    add_executable(
        ${_codec_test}
        ${CMAKE_CURRENT_SOURCE_DIR}/test/codecs/${_codec_test}.cpp
    )
    target_link_libraries(
        ${_codec_test}
        PRIVATE
            lux::cxx::algorithm
            lux::engine::resource::asset
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
    asset_content_public_link_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/codecs/asset_content_public_link_test.cpp
)
target_link_libraries(
    asset_content_public_link_test
    PRIVATE lux::engine::resource::asset
)
lux_classify_target(
    TARGET  asset_content_public_link_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    DOMAIN
)

set(ENABLE_ASSET_TEST OFF CACHE BOOL "Enable asset module tests")
if(ENABLE_ASSET_TEST)
    add_subdirectory(test)
endif()
