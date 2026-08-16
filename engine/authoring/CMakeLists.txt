add_subdirectory(project)
add_subdirectory(flowforge)
add_subdirectory(assets)
add_subdirectory(world)

install_components(
    PROJECT_NAME    lux-engine-authoring
    VERSION         0.0.1
    NAMESPACE       lux::engine::authoring
    COMPONENTS      project authoring_flowforge authoring_assets authoring_world
)
