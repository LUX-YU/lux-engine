## engine::extensions::extension_api — Engine-owned Extension API boundary.
##
## CORE-001 creates the upper-layer target before the ABI headers move out of
## modules/core. The old extension_abi component remains the compatibility
## implementation until CORE-002..005 migrate its headers and consumers.

add_interface_component(
    COMPONENT_NAME  extension_api
    NAMESPACE       lux::engine::extensions
    OUTPUT_NAME     lux_engine_extensions_api
)

component_include_directories(
    extension_api
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME
        include
)

target_link_libraries(
    extension_api
    INTERFACE
        lux::engine::core::extension_abi
)

lux_classify_target(
    TARGET  extension_api
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

component_add_transitive_commands(
    extension_api
    "find_package(lux-engine-core REQUIRED COMPONENTS extension_abi)"
)

install_components(
    PROJECT_NAME lux-engine-extensions
    VERSION      0.0.1
    NAMESPACE    lux::engine::extensions
    COMPONENTS   extension_api
)
