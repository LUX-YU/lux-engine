add_subdirectory(launch)
add_subdirectory(extensions)

add_component(COMPONENT_NAME editor_bootstrap NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/EditorApplication.cpp src/EditorViews.cpp src/EditorCommands.cpp src/EditorLifecycle.cpp src/EditorWorkspace.cpp src/EditorContent.cpp src/EditorViewClosure.cpp src/EditorSceneTools.cpp src/EditorSaving.cpp src/EditorReload.cpp src/EditorResults.cpp)
component_include_directories(editor_bootstrap BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude INSTALL_TIME include)
target_link_libraries(editor_bootstrap PUBLIC lux::engine::editor::session_execution lux::engine::editor::editor_project
    PRIVATE lux::engine::engine_context_render lux::engine::project::project_plugin_rendering
    lux::engine::editor::editor_extensions lux::engine::editor::desktop_shell
    lux::engine::editor::editor_assets lux::engine::editor::editor_storage
    lux::engine::editor::editor_file_publication lux::engine::editor::editor_persistence_execution
    lux::engine::editor::workspace_store lux::engine::editor::project_ui lux::engine::editor::tasks_ui imgui::core)
component_add_transitive_commands(editor_bootstrap
    "find_package(lux-engine-editor-session-factories REQUIRED COMPONENTS session_execution)"
    "find_package(lux-engine-editor-project REQUIRED COMPONENTS editor_project)"
    "find_package(lux-engine-context-render REQUIRED COMPONENTS engine_context_render)"
    "find_package(lux-engine-project-plugin-rendering REQUIRED COMPONENTS project_plugin_rendering)"
    "find_package(lux-engine-editor-extensions REQUIRED COMPONENTS editor_extensions)"
    "find_package(lux-engine-editor-desktop REQUIRED COMPONENTS desktop_shell)"
    "find_package(lux-engine-editor-assets REQUIRED COMPONENTS editor_assets)"
    "find_package(lux-engine-editor-storage REQUIRED COMPONENTS editor_storage)"
    "find_package(lux-engine-editor-file-publication REQUIRED COMPONENTS editor_file_publication)"
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence_execution)"
    "find_package(lux-engine-editor-workspace-store REQUIRED COMPONENTS workspace_store)"
    "find_package(lux-engine-editor-project-ui REQUIRED COMPONENTS project_ui)"
    "find_package(lux-engine-editor-tasks-ui REQUIRED COMPONENTS tasks_ui)"
    "find_package(imgui REQUIRED COMPONENTS core)")
lux_classify_target(TARGET editor_bootstrap LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-application VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_bootstrap)
