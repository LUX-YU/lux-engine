add_interface_component(
    COMPONENT_NAME navigation
    NAMESPACE      lux::engine::function
)

lux_classify_target(
    TARGET  navigation
    LAYER   FUNCTION
    PRODUCT RUNTIME
    ROLE    DOMAIN
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
        lux::engine::resource::spatial
)

component_add_transitive_commands(
    navigation
    "find_package(lux-engine-resource REQUIRED COMPONENTS spatial)"
)

add_subdirectory(test)
