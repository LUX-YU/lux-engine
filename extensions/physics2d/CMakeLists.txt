add_subdirectory(simulation)

install_components(
    PROJECT_NAME lux-engine-physics2d
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::physics2d
    COMPONENTS   physics2d_simulation
)
