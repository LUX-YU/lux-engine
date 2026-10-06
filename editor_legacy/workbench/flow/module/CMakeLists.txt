# Product/plugin composition owns declarations; ordinary UI consumers do not import the plugin protocol.
add_component(COMPONENT_NAME flow_module_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/FlowModule.cpp)
component_include_directories(flow_module_legacy BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(flow_module_legacy PUBLIC lux::engine::editor::editor_extensions_legacy
    PRIVATE lux::engine::editor::flow_ui_legacy lux::engine::editor::flowforge_persistence_legacy)
component_add_transitive_commands(flow_module_legacy
    "find_package(lux-engine-editor-extensions REQUIRED COMPONENTS editor_extensions_legacy)"
    "find_package(lux-engine-editor-flow-ui REQUIRED COMPONENTS flow_ui_legacy)"
    "find_package(lux-engine-editor-flowforge-persistence REQUIRED COMPONENTS flowforge_persistence_legacy)")
lux_classify_target(TARGET flow_module_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-flow-module VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS flow_module_legacy)
engine_declare_editor_module(TARGET flow_module_legacy
    HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/flowforge/FlowModule.hpp"
    LOGICAL_PATH lux/engine/editor/flowforge/FlowModule.hpp
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/FlowModule.cpp")
