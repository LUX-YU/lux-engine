set(_asset_root ${CMAKE_CURRENT_LIST_DIR}/..)

# UUID and persisted type vocabulary are cheap leaf values used by ECS
# components and manifests.  They must not pull the asset ledger, codecs or
# pak reader into a consumer's link closure.
add_interface_component(
    COMPONENT_NAME asset_identity
    NAMESPACE      lux::engine::resource
)
component_include_directories(
    asset_identity
    BUILD_TIME_EXPORT
        ${_asset_root}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_link_libraries(asset_identity INTERFACE stduuid)
component_add_transitive_commands(
    asset_identity
    "find_package(stduuid CONFIG REQUIRED)"
)
lux_classify_target(
    TARGET  asset_identity
    LAYER   RESOURCE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)
