add_component(COMPONENT_NAME flowforge_compilation NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/PublishFlowArtifact.cpp src/FlowCompilationService.cpp)
component_include_directories(flowforge_compilation BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(flowforge_compilation PUBLIC lux::engine::editor::editor_persistence lux::engine::editor::flowforge_model
    lux::engine::flowforge::flowforge_compiler lux::engine::process::process_execution)
component_add_transitive_commands(flowforge_compilation
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence)"
    "find_package(lux-engine-editor-flowforge-model REQUIRED COMPONENTS flowforge_model)"
    "find_package(lux-engine-flowforge-compiler REQUIRED COMPONENTS flowforge_compiler)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)")
lux_classify_target(TARGET flowforge_compilation LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-flowforge-compilation VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS flowforge_compilation)
