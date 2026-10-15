add_component(
    STATIC
    COMPONENT_NAME render_resource
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_resource
    SOURCE_FILES src/Storage.cpp
)

component_include_directories(
    render_resource
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    INSTALL_TIME include
)

component_add_internal_dependencies(render_resource lux::engine::render::render_data)

target_compile_features(render_resource PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
