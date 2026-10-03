find_package(imgui REQUIRED COMPONENTS core)
add_component(COMPONENT_NAME tasks_ui NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/TaskView.cpp)
component_include_directories(tasks_ui BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(tasks_ui PUBLIC lux::engine::editor::view_api lux::engine::editor::editor_tasks PRIVATE lux::engine::editor::view_host imgui::core)
component_add_transitive_commands(tasks_ui
    "find_package(lux-engine-editor-view-api REQUIRED COMPONENTS view_api)"
    "find_package(lux-engine-editor-tasks REQUIRED COMPONENTS editor_tasks)"
    "find_package(lux-engine-editor-desktop REQUIRED COMPONENTS view_host)"
    "find_package(imgui REQUIRED COMPONENTS core)")
target_include_directories(tasks_ui PRIVATE ${PROJECT_SOURCE_DIR}/editor/workbench/sinclude)
lux_classify_target(TARGET tasks_ui LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-tasks-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS tasks_ui)
