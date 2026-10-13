add_subdirectory(core)

lux_engine_install_components(
    PROJECT_NAME lux-engine-render
    VERSION 0.0.1
    NAMESPACE lux::engine::render
    COMPONENTS render_core
)
