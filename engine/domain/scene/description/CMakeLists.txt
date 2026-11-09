add_interface_component(
    COMPONENT_NAME scene_description
    NAMESPACE lux::engine::scene
)

component_include_directories(
    scene_description
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)

target_link_libraries(
    scene_description
    INTERFACE
        lux::engine::world::description
        lux::engine::simulation::description
        lux::engine::ecs::ecs_schema
        lux::engine::spatial::spatial3d_index
)

component_add_transitive_commands(
    scene_description
    "find_package(lux-engine-world-description REQUIRED COMPONENTS world_description)"
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_description)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_schema)"
    "find_package(lux-engine-domain-spatial3d REQUIRED COMPONENTS spatial3d_index)"
)

lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-description
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::scene
    COMPONENTS scene_description
)
