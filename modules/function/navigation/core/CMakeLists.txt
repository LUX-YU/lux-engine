add_interface_component(
    COMPONENT_NAME navigation
    NAMESPACE      lux::engine::function
)

component_include_directories(
    navigation
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)

target_link_libraries(
    navigation
    INTERFACE
        lux::engine::core::math
)

component_add_transitive_commands(
    navigation
    "find_package(lux-engine-core REQUIRED COMPONENTS math)"
)
