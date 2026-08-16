add_interface_component(
    COMPONENT_NAME extension_abi
    NAMESPACE      lux::engine::core
)

component_include_directories(
    extension_abi
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME
        include
)

component_add_transitive_commands(
    extension_abi
    "find_package(lux-cxx REQUIRED COMPONENTS core abi)"
)

target_link_libraries(
    extension_abi
    INTERFACE
        lux::cxx::core
        lux::cxx::abi
)

lux_classify_target(
    TARGET  extension_abi
    LAYER   CORE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)
