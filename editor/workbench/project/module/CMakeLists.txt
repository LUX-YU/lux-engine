# Composition is selectable independently of Scene/Material tools and the query-only browser.
add_component(COMPONENT_NAME project_module NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/ProjectModule.cpp)
component_include_directories(project_module BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(project_module PUBLIC lux::engine::editor::editor_extensions
    PRIVATE lux::engine::editor::project_tools_ui lux::engine::editor::project_ui lux::engine::editor::editor_storage lux::engine::editor::scene_project
    lux::engine::editor::editor_launch lux::engine::editor::editor_assets
    lux::engine::editor::session_execution lux::engine::editor::editor_persistence_execution
    lux::engine::editor::editor_file_publication)
component_add_transitive_commands(project_module
    "find_package(lux-engine-editor-extensions REQUIRED COMPONENTS editor_extensions)"
    "find_package(lux-engine-editor-project-ui REQUIRED COMPONENTS project_ui)"
    "find_package(lux-engine-editor-project-tools-ui REQUIRED COMPONENTS project_tools_ui)"
    "find_package(lux-engine-editor-launch REQUIRED COMPONENTS editor_launch)"
    "find_package(lux-engine-editor-assets REQUIRED COMPONENTS editor_assets)"
    "find_package(lux-engine-editor-storage REQUIRED COMPONENTS editor_storage)"
    "find_package(lux-engine-editor-scene-execution REQUIRED COMPONENTS scene_project)"
    "find_package(lux-engine-editor-session-factories REQUIRED COMPONENTS session_execution)"
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence_execution)"
    "find_package(lux-engine-editor-file-publication REQUIRED COMPONENTS editor_file_publication)")
lux_classify_target(TARGET project_module LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-project-module VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS project_module)
engine_declare_editor_module(TARGET project_module
    HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/project/ProjectModule.hpp"
    LOGICAL_PATH lux/engine/editor/project/ProjectModule.hpp
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/ProjectModule.cpp")
