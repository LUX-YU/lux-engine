add_component(COMPONENT_NAME project_plugin_rendering NAMESPACE lux::engine::project STATIC
    SOURCE_FILES src/PluginRendering.cpp)
component_include_directories(project_plugin_rendering BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../pinclude INSTALL_TIME include)
target_link_libraries(project_plugin_rendering PUBLIC lux::engine::project::project_plugins lux::engine::scene::scene_render)
component_add_transitive_commands(project_plugin_rendering
    "find_package(lux-engine-project-plugins REQUIRED COMPONENTS project_plugins)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)")
lux_classify_target(TARGET project_plugin_rendering LAYER SCENE PRODUCT SHARED ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-project-plugin-rendering VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::project COMPONENTS project_plugin_rendering)
