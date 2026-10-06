find_package(imgui REQUIRED COMPONENTS core)
add_component(COMPONENT_NAME tasks_ui_legacy NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/TaskView.cpp)
component_include_directories(tasks_ui_legacy BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(tasks_ui_legacy PUBLIC lux::engine::editor::editor_tasks_legacy lux::engine::editor::editor_composition_legacy PRIVATE imgui::core)
component_add_transitive_commands(tasks_ui_legacy
    "find_package(lux-engine-editor-tasks REQUIRED COMPONENTS editor_tasks_legacy)"
    "find_package(lux-engine-editor-composition REQUIRED COMPONENTS editor_composition_legacy)"
    "find_package(imgui REQUIRED COMPONENTS core)")
target_include_directories(tasks_ui_legacy PRIVATE ${PROJECT_SOURCE_DIR}/editor_legacy/workbench/sinclude)
lux_classify_target(TARGET tasks_ui_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-tasks-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS tasks_ui_legacy)
