add_component(COMPONENT_NAME flowforge_persistence NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/FlowSaveSource.cpp src/FlowCodec.cpp src/SessionFactory.cpp)
component_include_directories(flowforge_persistence
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(flowforge_persistence PUBLIC lux::engine::editor::flowforge_model lux::engine::editor::editor_persistence)
component_add_transitive_commands(flowforge_persistence
    "find_package(lux-engine-editor-flowforge-model REQUIRED COMPONENTS flowforge_model)"
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence)")
target_link_libraries(flowforge_persistence PUBLIC lux::engine::editor::session_factories
    PRIVATE lux::engine::core::services lux::engine::editor::editor_configuration)
target_include_directories(flowforge_persistence PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../sessions/sinclude)
component_add_transitive_commands(flowforge_persistence
    "find_package(lux-engine-editor-session-factories REQUIRED COMPONENTS session_factories)"
    "find_package(lux-engine-core REQUIRED COMPONENTS services)"
    "find_package(lux-engine-editor-configuration REQUIRED COMPONENTS editor_configuration)")
lux_classify_target(TARGET flowforge_persistence LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-flowforge-persistence VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS flowforge_persistence)

add_component(COMPONENT_NAME flowforge_compilation NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/PublishFlowArtifact.cpp src/FlowCompilationService.cpp)
component_include_directories(flowforge_compilation BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(flowforge_compilation PUBLIC lux::engine::editor::editor_persistence lux::engine::editor::flowforge_model
    lux::engine::flowforge::flowforge_compiler lux::engine::process::process_execution lux::engine::core::services)
component_add_transitive_commands(flowforge_compilation
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence)"
    "find_package(lux-engine-editor-flowforge-model REQUIRED COMPONENTS flowforge_model)"
    "find_package(lux-engine-flowforge-compiler REQUIRED COMPONENTS flowforge_compiler)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-core REQUIRED COMPONENTS services)")
lux_classify_target(TARGET flowforge_compilation LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-flowforge-compilation VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS flowforge_compilation)

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
