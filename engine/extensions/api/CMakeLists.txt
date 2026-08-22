## engine::extensions::extension_api — Engine-owned same-toolchain ABI v5 boundary.

find_package(lux-cxx REQUIRED COMPONENTS core abi)

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
        lux::cxx::core
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
    "find_package(lux-cxx REQUIRED COMPONENTS core abi)"
)

install_components(
    PROJECT_NAME lux-engine-extensions
    VERSION      0.0.1
    NAMESPACE    lux::engine::extensions
    COMPONENTS   extension_api
)
