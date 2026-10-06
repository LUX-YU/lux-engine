add_component(COMPONENT_NAME flowforge_persistence_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/FlowSaveSource.cpp src/FlowCodec.cpp src/SessionFactory.cpp)
component_include_directories(flowforge_persistence_legacy
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(flowforge_persistence_legacy PUBLIC lux::engine::editor::flowforge_model_legacy lux::engine::editor::editor_persistence_legacy)
component_add_transitive_commands(flowforge_persistence_legacy
    "find_package(lux-engine-editor-flowforge-model REQUIRED COMPONENTS flowforge_model_legacy)"
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence_legacy)")
target_link_libraries(flowforge_persistence_legacy PUBLIC lux::engine::editor::session_factories_legacy
    PRIVATE lux::engine::core::services lux::engine::editor::editor_configuration_legacy)
target_include_directories(flowforge_persistence_legacy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sessions/sinclude)
component_add_transitive_commands(flowforge_persistence_legacy
    "find_package(lux-engine-editor-session-factories REQUIRED COMPONENTS session_factories_legacy)"
    "find_package(lux-engine-core REQUIRED COMPONENTS services)"
    "find_package(lux-engine-editor-configuration REQUIRED COMPONENTS editor_configuration_legacy)")
lux_classify_target(TARGET flowforge_persistence_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-flowforge-persistence VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS flowforge_persistence_legacy)

add_component(COMPONENT_NAME flowforge_compilation_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/PublishFlowArtifact.cpp src/FlowCompilationService.cpp)
component_include_directories(flowforge_compilation_legacy BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(flowforge_compilation_legacy PUBLIC lux::engine::editor::editor_persistence_legacy lux::engine::editor::flowforge_model_legacy
    lux::engine::flowforge::flowforge_compiler lux::engine::process::process_execution lux::engine::core::services)
component_add_transitive_commands(flowforge_compilation_legacy
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence_legacy)"
    "find_package(lux-engine-editor-flowforge-model REQUIRED COMPONENTS flowforge_model_legacy)"
    "find_package(lux-engine-flowforge-compiler REQUIRED COMPONENTS flowforge_compiler)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-core REQUIRED COMPONENTS services)")
lux_classify_target(TARGET flowforge_compilation_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-flowforge-compilation VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS flowforge_compilation_legacy)

# Exact SDK ownership: sibling providers share a build include root, never an install manifest.
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/flowforge/FlowCompilationService.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/flowforge/PublishFlowArtifact.hpp
    DESTINATION include/lux/engine/editor/flowforge COMPONENT lux_engine_editor_flowforge_compilation)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/flowforge/FlowCodec.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/flowforge/FlowSaveSource.hpp
    DESTINATION include/lux/engine/editor/flowforge COMPONENT lux_engine_editor_flowforge_persistence)

install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/flowforge/FlowSessionFactory.hpp
    DESTINATION include/lux/engine/editor/flowforge COMPONENT lux_engine_editor_flowforge_persistence)
