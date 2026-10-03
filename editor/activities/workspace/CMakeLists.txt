add_component(COMPONENT_NAME workspace_store NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/WorkspaceStore.cpp src/LegacyWorkspaceImporter.cpp src/WorkspaceMigration.cpp)
component_include_directories(workspace_store BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(workspace_store PUBLIC lux::engine::editor::layout_model lux::engine::editor::editor_persistence
    PRIVATE lux::engine::editor::editor_file_publication)
find_package(tomlplusplus CONFIG REQUIRED)
target_compile_definitions(workspace_store PRIVATE TOML_HEADER_ONLY=1 TOML_EXCEPTIONS=0)
target_include_directories(workspace_store PRIVATE $<TARGET_PROPERTY:tomlplusplus::tomlplusplus,INTERFACE_INCLUDE_DIRECTORIES>)
component_add_transitive_commands(workspace_store
    "find_package(lux-engine-editor-layout REQUIRED COMPONENTS layout_model)"
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence)"
    "find_package(lux-engine-editor-file-publication REQUIRED COMPONENTS editor_file_publication)")
lux_classify_target(TARGET workspace_store LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-workspace-store VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS workspace_store)
