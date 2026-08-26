add_interface_component(
    COMPONENT_NAME ecs_task
    NAMESPACE      lux::engine::simulation::ecs
)
add_library(
    lux::engine::simulation::ecs::task
    ALIAS ecs_task
)
component_include_directories(
    ecs_task
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)
target_link_libraries(
    ecs_task
    INTERFACE
        lux::engine::simulation::ecs::core
        lux::engine::simulation::system
        lux::engine::core::task
)
component_add_internal_dependencies(ecs_task core)
component_add_transitive_commands(
    ecs_task
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-core REQUIRED COMPONENTS task)"
)
lux_classify_target(
    TARGET  ecs_task
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)
