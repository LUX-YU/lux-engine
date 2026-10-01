find_package(imgui REQUIRED COMPONENTS core)
add_component(COMPONENT_NAME project_ui NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/AssetPickerElement.cpp src/ProjectView.cpp)
component_include_directories(project_ui BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(project_ui PUBLIC lux::engine::editor::view_api lux::engine::editor::editor_project
    lux::engine::function::ui PRIVATE imgui::core)
component_add_transitive_commands(project_ui
    "find_package(lux-engine-editor-view-api REQUIRED COMPONENTS view_api)"
    "find_package(lux-engine-editor-project REQUIRED COMPONENTS editor_project)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package(imgui REQUIRED COMPONENTS core)")
lux_classify_target(TARGET project_ui LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-project-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS project_ui)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_project_views_test test/catalog.cpp)
    target_link_libraries(editor_project_views_test PRIVATE project_ui desktop_shell)
    lux_editor_test_options(editor_project_views_test)
    add_test(NAME editor.project_views COMMAND editor_project_views_test)
endif()
