find_package(stduuid CONFIG REQUIRED)

add_interface_component(
    COMPONENT_NAME identity
    NAMESPACE      lux::engine::resource
)

component_include_directories(
    identity
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)

target_link_libraries(
    identity
    INTERFACE stduuid
)

component_add_transitive_commands(
    identity
    "find_package(stduuid CONFIG REQUIRED)"
)

lux_classify_target(
    TARGET  identity
    LAYER   RESOURCE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)
