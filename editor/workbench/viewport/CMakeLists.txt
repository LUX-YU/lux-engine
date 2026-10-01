find_package(imgui REQUIRED COMPONENTS core)
add_component(COMPONENT_NAME editor_viewport NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/ViewportElement.cpp src/ViewportPresentation.cpp src/CameraNavigation.cpp)
component_include_directories(editor_viewport BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(editor_viewport PUBLIC lux::engine::scene::scene_composition lux::engine::scene::scene_render
    lux::engine::function::ui PRIVATE imgui::core)
component_add_transitive_commands(editor_viewport
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package(imgui REQUIRED COMPONENTS core)")
lux_classify_target(TARGET editor_viewport LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-viewport VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_viewport)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_highlight_p07_test test/highlight.cpp)
    target_link_libraries(editor_highlight_p07_test PRIVATE editor_viewport)
    lux_editor_test_options(editor_highlight_p07_test)
    add_test(NAME editor.projection.highlight_backend COMMAND editor_highlight_p07_test)
    set_tests_properties(editor.projection.highlight_backend PROPERTIES LABELS "editor;native;P07")
endif()
