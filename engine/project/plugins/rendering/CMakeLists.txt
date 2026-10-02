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

if(BUILD_TESTING)
    add_executable(plugin_scene_registrations_test test/scene_registrations.cpp)
    target_link_libraries(plugin_scene_registrations_test PRIVATE project_plugin_rendering)
    if(MSVC)
        target_compile_options(plugin_scene_registrations_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(plugin_scene_registrations_test PRIVATE -UNDEBUG)
    endif()
    add_test(NAME plugin.scene_registrations COMMAND plugin_scene_registrations_test
        "${CMAKE_BINARY_DIR}/share/lux-engine/plugins/catalog.json" "${CMAKE_BINARY_DIR}")
    set_tests_properties(plugin.scene_registrations PROPERTIES LABELS "native;P12" TIMEOUT 30)
endif()
