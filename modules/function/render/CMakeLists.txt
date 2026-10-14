add_subdirectory(core)

add_subdirectory(data)

add_subdirectory(protocol)

add_subdirectory(graph)

add_subdirectory(features)

add_subdirectory(native/vulkan)

lux_engine_install_components(
    PROJECT_NAME lux-engine-render
    VERSION 0.0.1
    NAMESPACE lux::engine::render
    COMPONENTS render_core render_data render_protocol render_graph render_features render_native_vulkan
)
