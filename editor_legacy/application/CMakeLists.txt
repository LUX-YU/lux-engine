
add_component(COMPONENT_NAME editor_bootstrap_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/EditorStartup.cpp src/EditorApplication.cpp src/EditorViews.cpp src/EditorCommands.cpp src/EditorLifecycle.cpp src/EditorWorkspace.cpp src/EditorContent.cpp src/EditorViewClosure.cpp src/EditorSceneTools.cpp src/EditorSaving.cpp src/EditorResults.cpp src/EditorRecovery.cpp src/EditorProjectTools.cpp src/EditorSettings.cpp src/Launcher.cpp)
component_include_directories(editor_bootstrap_legacy BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${CMAKE_CURRENT_SOURCE_DIR}/sinclude INSTALL_TIME include)
target_link_libraries(editor_bootstrap_legacy PUBLIC lux::engine::platform::window lux::engine::editor::session_execution_legacy lux::engine::editor::editor_project_legacy lux::engine::editor::editor_commands_legacy lux::engine::editor::editor_extensions_legacy
    PRIVATE lux::engine::editor::scene_project_legacy lux::engine::platform::platform_process lux::engine::editor::editor_launch_legacy lux::engine::engine_context_render lux::engine::project::project_plugin_rendering
    lux::engine::editor::editor_extensions_legacy lux::engine::editor::scene_ui_legacy lux::engine::editor::scene_persistence_legacy lux::engine::editor::desktop_shell_legacy
    lux::engine::editor::editor_assets_legacy lux::engine::editor::editor_storage_legacy
    lux::engine::editor::editor_file_publication_legacy lux::engine::editor::editor_persistence_execution_legacy
    lux::engine::editor::project_tools_ui_legacy lux::engine::editor::workspace_store_legacy lux::engine::editor::project_ui_legacy lux::engine::editor::tasks_ui_legacy imgui::core)
component_add_transitive_commands(editor_bootstrap_legacy
    "find_package(lux-engine-editor-scene-execution REQUIRED COMPONENTS scene_project_legacy)"
    "find_package(lux-engine-platform REQUIRED COMPONENTS platform_process window)"
    "find_package(lux-engine-editor-commands REQUIRED COMPONENTS editor_commands_legacy)"
    "find_package(lux-engine-editor-launch REQUIRED COMPONENTS editor_launch_legacy)"
    "find_package(lux-engine-editor-session-factories REQUIRED COMPONENTS session_execution_legacy)"
    "find_package(lux-engine-editor-project REQUIRED COMPONENTS editor_project_legacy)"
    "find_package(lux-engine-context-render REQUIRED COMPONENTS engine_context_render)"
    "find_package(lux-engine-project-plugin-rendering REQUIRED COMPONENTS project_plugin_rendering)"
    "find_package(lux-engine-editor-extensions REQUIRED COMPONENTS editor_extensions_legacy)"
    "find_package(lux-engine-editor-scene-ui REQUIRED COMPONENTS scene_ui_legacy)"
    "find_package(lux-engine-editor-scene-persistence REQUIRED COMPONENTS scene_persistence_legacy)"
    "find_package(lux-engine-editor-desktop REQUIRED COMPONENTS desktop_shell_legacy)"
    "find_package(lux-engine-editor-assets REQUIRED COMPONENTS editor_assets_legacy)"
    "find_package(lux-engine-editor-storage REQUIRED COMPONENTS editor_storage_legacy)"
    "find_package(lux-engine-editor-file-publication REQUIRED COMPONENTS editor_file_publication_legacy)"
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence_execution_legacy)"
    "find_package(lux-engine-editor-workspace-store REQUIRED COMPONENTS workspace_store_legacy)"
    "find_package(lux-engine-editor-project-ui REQUIRED COMPONENTS project_ui_legacy)"
    "find_package(lux-engine-editor-project-tools-ui REQUIRED COMPONENTS project_tools_ui_legacy)"
    "find_package(lux-engine-editor-tasks-ui REQUIRED COMPONENTS tasks_ui_legacy)"
    "find_package(imgui REQUIRED COMPONENTS core)")
target_compile_definitions(editor_bootstrap_legacy PRIVATE LUX_EDITOR_VERSION="${PROJECT_VERSION}" TOML_HEADER_ONLY=1 TOML_EXCEPTIONS=0)
lux_classify_target(TARGET editor_bootstrap_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-application VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_bootstrap_legacy)

find_package(lux-cxx REQUIRED COMPONENTS arguments)
add_executable(lux_editor_legacy main.cpp)
add_executable(lux_launcher_legacy launcher_main.cpp)
foreach(product lux_editor_legacy lux_launcher_legacy)
    target_include_directories(${product} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude)
    target_link_libraries(${product} PRIVATE editor_bootstrap_legacy lux::engine::platform::platform_process lux::cxx::arguments)
    lux_classify_target(TARGET ${product} LAYER EDITOR PRODUCT EDITOR ROLE EXECUTABLE)
    install(TARGETS ${product} RUNTIME DESTINATION bin)
    if(WIN32)
        install(FILES $<TARGET_RUNTIME_DLLS:${product}> DESTINATION bin)
    endif()
endforeach()

# The executable selects domain declarations; the reusable host has no built-in module fallback.
set(LUX_EDITOR_PRODUCT_MODULES "project_module_legacy;flow_module_legacy;material_module_legacy;scene_module_legacy" CACHE STRING "Selected Editor module providers")
engine_target_add_editor_modules(TARGET lux_editor_legacy NAME productModules NAMESPACE lux::editor::product
    MODULES ${LUX_EDITOR_PRODUCT_MODULES})
