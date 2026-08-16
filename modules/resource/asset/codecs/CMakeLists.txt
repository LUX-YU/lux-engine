set(_asset_root ${CMAKE_CURRENT_LIST_DIR}/..)

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
    PUBLIC lux::engine::resource::asset_core
    PRIVATE lux::engine::platform::common
)
component_add_internal_dependencies(asset_codecs description)
component_add_cmake_scripts(
    asset_codecs
    ${_asset_root}/cmake/add_asset.cmake
)
component_add_transitive_commands(
    asset_codecs
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset_core description)"
)
lux_classify_target(
    TARGET  asset_codecs
    LAYER   RESOURCE
    PRODUCT RUNTIME
    ROLE    DOMAIN
)
