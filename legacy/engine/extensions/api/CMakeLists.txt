## Engine-owned extension contracts.  Identity is a FUNCTION-layer semantic
## contract shared by Authoring and the ABI; the same-toolchain ABI remains a
## Runtime boundary.

find_package(lux-cxx REQUIRED COMPONENTS core abi)

add_interface_component(
    COMPONENT_NAME  extension_identity
    NAMESPACE       lux::engine::extensions
    OUTPUT_NAME     lux_engine_extension_identity
)

component_include_directories(
    extension_identity
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME
        include
)

target_link_libraries(
    extension_identity
    INTERFACE
        lux::cxx::core
)

lux_classify_target(
    TARGET  extension_identity
    LAYER   FUNCTION
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

component_add_transitive_commands(
    extension_identity
    "find_package(lux-cxx REQUIRED COMPONENTS core)"
)

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
        lux::engine::extensions::extension_identity
        lux::cxx::abi
)

lux_classify_target(
    TARGET  extension_api
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

component_add_transitive_commands(
    extension_api
    "find_package(lux-cxx REQUIRED COMPONENTS abi)"
    "find_package(lux-engine-extensions REQUIRED COMPONENTS extension_identity)"
)

install_components(
    PROJECT_NAME lux-engine-extensions
    VERSION      0.0.1
    NAMESPACE    lux::engine::extensions
    COMPONENTS   extension_identity extension_api
)
