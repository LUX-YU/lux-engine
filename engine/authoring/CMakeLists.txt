add_subdirectory(script)

install_components(
    PROJECT_NAME lux-engine-authoring
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::authoring
    COMPONENTS
        authoring_script
)
