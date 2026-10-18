add_component(
    STATIC
    COMPONENT_NAME render_graph
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_graph
    SOURCE_FILES src/Builder.cpp src/Analysis.cpp
)

component_include_directories(
    render_graph
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/pinclude"
    INSTALL_TIME include
)

component_add_internal_dependencies(render_graph
    lux::engine::render::render_core
    lux::engine::render::render_data
)

target_compile_features(render_graph PUBLIC cxx_std_20)
