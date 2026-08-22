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
)

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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/AssetCodecCatalog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/AssetLoadPort.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/AssetManager.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/AssetSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/AssetSerDeserFactory.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/animation/AnimationClipAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/animation/AnimationClipDescriptionCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/animation/AnimationClipSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/animation/SkeletonAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/animation/SkeletonDescriptionCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/animation/SkeletonSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/material/MaterialAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/material/MaterialInstanceAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/material/MaterialInstanceSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/material/MaterialSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mesh/MeshAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mesh/MeshDescriptionCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mesh/MeshSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/model/ModelAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/model/ModelSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/script/ScriptAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/script/ScriptDescriptionCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/script/ScriptSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/ShaderAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/ShaderSerDeser.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/AssetVfs.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/VirtualPath.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/pak/PakArchive.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/pak/PakAssetProvider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/pak/PakCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/texture/TextureAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/texture/TextureAtlasAssets.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/texture/TextureAtlasAssetValues.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/texture/TextureSerDeser.cpp
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
        lux::cxx::algorithm
        lux::cxx::compile_time
        lux::cxx::memory
        lux::engine::core::math
        lux::engine::core::async_port
        lux::engine::resource::description
    PRIVATE
        lux::cxx::core
        lux::engine::core::serialization
)

component_add_internal_dependencies(asset description)
component_add_cmake_scripts(
    asset
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/add_asset.cmake
)
component_add_transitive_commands(
    asset
    "find_package(stduuid CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time memory)"
    "find_package(lux-engine-core REQUIRED COMPONENTS async_port math)"
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

add_executable(
    pak_archive_public_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/storage/pak_archive_public_test.cpp
)
target_link_libraries(
    pak_archive_public_test
    PRIVATE lux::engine::resource::asset
)
lux_classify_target(
    TARGET  pak_archive_public_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    DOMAIN
)

add_executable(
    asset_storage_contract_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/storage/asset_storage_contract_test.cpp
)
target_link_libraries(
    asset_storage_contract_test
    PRIVATE lux::engine::resource::asset
)
lux_classify_target(
    TARGET  asset_storage_contract_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    DOMAIN
)

add_executable(
    asset_public_link_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/asset_public_link_test.cpp
)
target_link_libraries(
    asset_public_link_test
    PRIVATE lux::engine::resource::asset
)
lux_classify_target(
    TARGET  asset_public_link_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    DOMAIN
)

add_executable(
    asset_wire_contract_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/asset_wire_contract_test.cpp
)
target_link_libraries(
    asset_wire_contract_test
    PRIVATE lux::engine::resource::asset
)
lux_classify_target(
    TARGET  asset_wire_contract_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    DOMAIN
)

set(ENABLE_ASSET_TEST OFF CACHE BOOL "Enable asset module tests")
if(ENABLE_ASSET_TEST)
    add_subdirectory(test)
endif()
