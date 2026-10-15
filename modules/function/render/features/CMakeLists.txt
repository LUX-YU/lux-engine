add_component(
    STATIC
    COMPONENT_NAME render_features
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_features
    SOURCE_FILES src/Assemble.cpp src/Texture.cpp
)

component_include_directories(
    render_features
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    INSTALL_TIME include
)

component_add_internal_dependencies(
    render_features
    lux::engine::render::render_graph
    lux::engine::render::render_data
    lux::engine::render::render_protocol
    lux::engine::render::render_resource
)

target_compile_features(render_features PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
