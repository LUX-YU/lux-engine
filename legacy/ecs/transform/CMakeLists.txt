# lux::engine::ecs::transform — the TransformSystem folder (behavior half of the
# data/behavior split, ecs-layer ADR §4): the 2D/3D bindings of the shared
# hierarchical local→world resolver to the ecs/components Transform pair.
#
# The resolution KERNEL (HierarchicalTransformSystem: preorder linear pass, G-07
# derived maintenance, cycle repair) stays in ecs/core — it is generic mechanism
# consumed beyond these bindings (editor load-time cycle guard, Camera2DSystem's
# maintainDerived). Header-only. namespace: lux::ecs

add_interface_component(
    COMPONENT_NAME transform
    NAMESPACE      lux::engine::ecs
)

lux_classify_target(
    TARGET transform
    LAYER ECS
    PRODUCT RUNTIME
    ROLE DOMAIN
)

component_include_directories(
    transform
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME
        include
)

target_link_libraries(
    transform
    INTERFACE
    lux::engine::ecs::core         # HierarchicalTransformSystem / World / ISystem
)

component_add_transitive_commands(
    transform
    "find_package (lux-engine-ecs REQUIRED COMPONENTS core)"
)

add_subdirectory(test)
