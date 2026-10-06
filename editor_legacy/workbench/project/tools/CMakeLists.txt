# Query-only project_ui_legacy stays usable by field pickers without import/toolchain dependencies.
add_component(COMPONENT_NAME project_tools_ui_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/ProjectCreation.cpp src/ContentViews.cpp src/ContentReview.cpp src/RestoreWorkbench.cpp src/WindowSettingsBinding.cpp src/DesktopSettingsControls.cpp src/SettingsContent.cpp src/DesktopSettings.cpp src/RecentProjectsView.cpp src/ImportView.cpp src/SettingsView.cpp src/ProjectCreationView.cpp src/ResultsView.cpp src/WorkspaceView.cpp)
component_include_directories(project_tools_ui_legacy BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(project_tools_ui_legacy PUBLIC lux::engine::editor::editor_composition_legacy
    lux::engine::editor::editor_assets_legacy
    lux::engine::editor::editor_project_legacy lux::engine::editor::scene_ui_legacy lux::engine::editor::desktop_shell_legacy
    lux::engine::editor::session_execution_legacy
    PRIVATE lux::engine::editor::project_ui_legacy lux::engine::core::log lux::engine::editor::editor_launch_legacy lux::engine::project::project_plugin_rendering lux::engine::editor::editor_extensions_legacy lux::engine::editor::editor_storage_legacy imgui::core)
component_add_transitive_commands(project_tools_ui_legacy
    "find_package(lux-engine-editor-project-ui REQUIRED COMPONENTS project_ui_legacy)"
    "find_package(lux-engine-core REQUIRED COMPONENTS log)"
    "find_package(lux-engine-editor-launch REQUIRED COMPONENTS editor_launch_legacy)"
    "find_package(lux-engine-project-plugin-rendering REQUIRED COMPONENTS project_plugin_rendering)"
    "find_package(lux-engine-editor-composition REQUIRED COMPONENTS editor_composition_legacy)"
    "find_package(lux-engine-editor-scene-ui REQUIRED COMPONENTS scene_ui_legacy)"
    "find_package(lux-engine-editor-assets REQUIRED COMPONENTS editor_assets_legacy)"
    "find_package(lux-engine-editor-project REQUIRED COMPONENTS editor_project_legacy)"
    "find_package(lux-engine-editor-desktop REQUIRED COMPONENTS desktop_shell_legacy)"
    "find_package(lux-engine-editor-extensions REQUIRED COMPONENTS editor_extensions_legacy)"
    "find_package(lux-engine-editor-storage REQUIRED COMPONENTS editor_storage_legacy)"
    "find_package(lux-engine-editor-session-factories REQUIRED COMPONENTS session_execution_legacy)"
    "find_package(imgui REQUIRED COMPONENTS core)")
target_include_directories(project_tools_ui_legacy PRIVATE ${PROJECT_SOURCE_DIR}/editor_legacy/workbench/sinclude)
lux_classify_target(TARGET project_tools_ui_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-project-tools-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS project_tools_ui_legacy)
