add_component(COMPONENT_NAME editor_launch NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/LaunchEditor.cpp)
component_include_directories(editor_launch BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(editor_launch PUBLIC lux::engine::editor::editor_contracts
    PRIVATE lux::engine::platform::platform_process lux::engine::process::process_execution lux::engine::core::services)
component_add_transitive_commands(editor_launch
    "find_package(lux-engine-editor-contracts REQUIRED COMPONENTS editor_contracts)"
    "find_package(lux-engine-platform REQUIRED COMPONENTS platform_process)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-core REQUIRED COMPONENTS services)")
lux_classify_target(TARGET editor_launch LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-launch VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_launch)
