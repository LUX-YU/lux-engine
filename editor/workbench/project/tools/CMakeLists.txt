# Query-only project_ui stays usable by field pickers without import/toolchain dependencies.
add_component(COMPONENT_NAME project_tools_ui NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/RecentProjectsView.cpp src/ImportView.cpp src/SettingsView.cpp src/ProjectCreationView.cpp src/ResultsView.cpp src/WorkspaceView.cpp)
component_include_directories(project_tools_ui BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(project_tools_ui PUBLIC lux::engine::editor::view_api lux::engine::editor::editor_assets
    lux::engine::editor::editor_project lux::engine::editor::scene_ui PRIVATE imgui::core)
component_add_transitive_commands(project_tools_ui
    "find_package(lux-engine-editor-scene-ui REQUIRED COMPONENTS scene_ui)"
    "find_package(lux-engine-editor-view-api REQUIRED COMPONENTS view_api)"
    "find_package(lux-engine-editor-assets REQUIRED COMPONENTS editor_assets)"
    "find_package(lux-engine-editor-project REQUIRED COMPONENTS editor_project)"
    "find_package(imgui REQUIRED COMPONENTS core)")
lux_classify_target(TARGET project_tools_ui LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-project-tools-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS project_tools_ui)
