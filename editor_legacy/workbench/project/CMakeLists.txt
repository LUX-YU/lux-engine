find_package(imgui REQUIRED COMPONENTS core)
add_component(COMPONENT_NAME project_ui_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/AssetPickerElement.cpp src/ProjectView.cpp)
component_include_directories(project_ui_legacy BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(project_ui_legacy PUBLIC lux::engine::editor::editor_project_legacy
    lux::engine::function::ui lux::engine::editor::editor_composition_legacy PRIVATE imgui::core)
component_add_transitive_commands(project_ui_legacy
    "find_package(lux-engine-editor-composition REQUIRED COMPONENTS editor_composition_legacy)"
    "find_package(lux-engine-editor-project REQUIRED COMPONENTS editor_project_legacy)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package(imgui REQUIRED COMPONENTS core)")
target_include_directories(project_ui_legacy PRIVATE ${PROJECT_SOURCE_DIR}/editor_legacy/workbench/sinclude)
lux_classify_target(TARGET project_ui_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-project-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS project_ui_legacy)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_project_views_test_legacy test/catalog.cpp)
    target_link_libraries(editor_project_views_test_legacy PRIVATE project_ui_legacy)
    target_include_directories(editor_project_views_test_legacy PRIVATE ${PROJECT_SOURCE_DIR}/cmake/test-support)
    lux_editor_test_options(editor_project_views_test_legacy)
    add_test(NAME editor.project_views COMMAND editor_project_views_test_legacy)
endif()

add_subdirectory(tools)
