set(_asset_root ${CMAKE_CURRENT_LIST_DIR}/..)

add_component(
    COMPONENT_NAME asset_core
    NAMESPACE      lux::engine::resource
    OUTPUT_NAME    lux_engine_asset_core
    SOURCE_FILES
        src/AssetManager.cpp
        src/AssetCodecCatalog.cpp
        src/AssetSerDeser.cpp
        src/AssetVfs.cpp
        src/VirtualPath.cpp
        src/MeshAsset.cpp
        src/MaterialAsset.cpp
        src/MaterialInstanceAsset.cpp
        src/ModelAsset.cpp
        src/ShaderAsset.cpp
        src/ScriptAsset.cpp
        src/TextureAsset.cpp
        src/SkeletonAsset.cpp
        src/AnimationClipAsset.cpp
        src/TextureAtlasAssetValues.cpp
)
component_include_directories(
    asset_core
    BUILD_TIME_EXPORT
        ${_asset_root}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${_asset_root}/pinclude
)
target_compile_definitions(asset_core PRIVATE LUX_RESOURCE_LIBRARY)
target_link_libraries(
    asset_core
    PUBLIC
        lux::engine::resource::asset_identity
        lux::cxx::memory
        lux::engine::core::math
        lux::engine::core::meta
        lux::engine::core::serialization
    PRIVATE
        lux::cxx::reflection_runtime
        lux::engine::platform::common
)
component_add_internal_dependencies(asset_core description)
component_add_transitive_commands(
    asset_core
    "find_package(stduuid CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS memory)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta math serialization)"
)
lux_classify_target(
    TARGET  asset_core
    LAYER   RESOURCE
    PRODUCT RUNTIME
    ROLE    DOMAIN
)
