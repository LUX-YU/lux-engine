set(_asset_root ${CMAKE_CURRENT_LIST_DIR}/..)

add_component(
    COMPONENT_NAME asset_pak
    NAMESPACE      lux::engine::resource
    OUTPUT_NAME    lux_engine_asset_pak
    SOURCE_FILES
        src/PakCodec.cpp
        src/PakAssetProvider.cpp
)
component_include_directories(
    asset_pak
    BUILD_TIME_EXPORT
        ${_asset_root}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
    PRIVATE ${_asset_root}/pinclude
)
target_compile_definitions(asset_pak PRIVATE LUX_ASSET_PAK_LIBRARY)
target_link_libraries(
    asset_pak
    PUBLIC lux::engine::resource::asset_core
    PRIVATE lux::cxx::algorithm
)
component_add_transitive_commands(
    asset_pak
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset_core)"
)
lux_classify_target(
    TARGET  asset_pak
    LAYER   RESOURCE
    PRODUCT RUNTIME
    ROLE    DOMAIN
)
