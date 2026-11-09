add_interface_component(
    COMPONENT_NAME simulation_task
    NAMESPACE      lux::engine::simulation
)
component_include_directories(
    simulation_task
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)
target_link_libraries(
    simulation_task
    INTERFACE
        lux::engine::ecs::ecs_core
        lux::engine::simulation::system
        lux::engine::core::task
)

component_add_transitive_commands(
    simulation_task
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_core)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-core REQUIRED COMPONENTS task)"
)
