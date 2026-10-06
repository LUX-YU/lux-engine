# Composition is selectable independently of Scene/Material tools and the query-only browser.
add_component(COMPONENT_NAME project_module_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/ProjectModule.cpp)
component_include_directories(project_module_legacy BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(project_module_legacy PUBLIC lux::engine::editor::editor_extensions_legacy
    PRIVATE lux::engine::editor::project_tools_ui_legacy lux::engine::editor::project_ui_legacy lux::engine::editor::editor_storage_legacy lux::engine::editor::scene_project_legacy
    lux::engine::editor::editor_launch_legacy lux::engine::editor::editor_assets_legacy
    lux::engine::editor::session_execution_legacy lux::engine::editor::editor_persistence_execution_legacy
    lux::engine::editor::editor_file_publication_legacy)
component_add_transitive_commands(project_module_legacy
    "find_package(lux-engine-editor-extensions REQUIRED COMPONENTS editor_extensions_legacy)"
    "find_package(lux-engine-editor-project-ui REQUIRED COMPONENTS project_ui_legacy)"
    "find_package(lux-engine-editor-project-tools-ui REQUIRED COMPONENTS project_tools_ui_legacy)"
    "find_package(lux-engine-editor-launch REQUIRED COMPONENTS editor_launch_legacy)"
    "find_package(lux-engine-editor-assets REQUIRED COMPONENTS editor_assets_legacy)"
    "find_package(lux-engine-editor-storage REQUIRED COMPONENTS editor_storage_legacy)"
    "find_package(lux-engine-editor-scene-execution REQUIRED COMPONENTS scene_project_legacy)"
    "find_package(lux-engine-editor-session-factories REQUIRED COMPONENTS session_execution_legacy)"
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence_execution_legacy)"
    "find_package(lux-engine-editor-file-publication REQUIRED COMPONENTS editor_file_publication_legacy)")
lux_classify_target(TARGET project_module_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-project-module VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS project_module_legacy)
engine_declare_editor_module(TARGET project_module_legacy
    HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/project/ProjectModule.hpp"
    LOGICAL_PATH lux/engine/editor/project/ProjectModule.hpp
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/ProjectModule.cpp")
