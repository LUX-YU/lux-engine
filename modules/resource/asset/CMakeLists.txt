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
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Asset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CookedAssetImage.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/detail/CookedAssetWriter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/material/MaterialAssets.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/metadata/MetadataAssets.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/model/ModelAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/ShaderAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/texture/TextureAsset.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/texture/TextureAtlasAssets.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/AssetVfs.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/VirtualPath.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/pak/PakArchive.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/pak/PakAssetProvider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/storage/pak/PakCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/mesh/MeshDescriptionCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/animation/SkeletonDescriptionCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/animation/AnimationClipDescriptionCodec.cpp
)

component_add_cmake_scripts(
    asset
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/add_asset.cmake
)

component_include_directories(
    asset
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/sinclude
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)

target_compile_definitions(asset PRIVATE LUX_ASSET_LIBRARY)

target_link_libraries(
    asset
    PUBLIC
        lux::cxx::algorithm
        lux::cxx::compile_time
        lux::cxx::memory
        lux::engine::resource::identity
        lux::engine::resource::description
    PRIVATE
        lux::cxx::core
        lux::engine::core::serialization
)

component_add_transitive_commands(
    asset
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm compile_time memory)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity description)"
)

lux_classify_target(
    TARGET  asset
    LAYER   RESOURCE
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

