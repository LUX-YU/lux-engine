# Stable ECS identities shared by the kernel and the LXES wire format.
# This component is data-only: no EnTT registry, Engine, Extension, AssetStore,
# renderer, editor, or scene-lifecycle dependency may enter it.

find_package(stduuid CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS algorithm)

add_interface_component(
    COMPONENT_NAME identity
    NAMESPACE      lux::engine::ecs
)

component_include_directories(
    identity
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)

target_link_libraries(
    identity
    INTERFACE
        lux::cxx::algorithm
        stduuid
)

component_add_transitive_commands(
    identity
    "find_package(stduuid CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS algorithm)"
)

lux_classify_target(
    TARGET  identity
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)
