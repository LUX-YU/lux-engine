add_component(
    STATIC
    COMPONENT_NAME render_graph
    NAMESPACE lux::engine::render
    OUTPUT_NAME lux_engine_render_graph
    SOURCE_FILES src/Definition.cpp src/Plan.cpp src/Bindings.cpp src/Builder.cpp
)

component_include_directories(
    render_graph
    BUILD_TIME_EXPORT "${CMAKE_CURRENT_SOURCE_DIR}/include"
    BUILD_TIME_SHARED "${CMAKE_CURRENT_SOURCE_DIR}/../../../resource/description/include"
    INSTALL_TIME include
)

target_link_libraries(render_graph PUBLIC lux::engine::render::render_core)

target_compile_features(render_graph PUBLIC cxx_std_20)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
