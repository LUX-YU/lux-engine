# Product/plugin composition owns declarations; ordinary UI consumers do not import the plugin protocol.
add_component(COMPONENT_NAME material_module_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/MaterialModule.cpp)
component_include_directories(material_module_legacy BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(material_module_legacy PUBLIC lux::engine::editor::editor_extensions_legacy
    PRIVATE lux::engine::editor::material_ui_legacy lux::engine::editor::material_persistence_legacy)
component_add_transitive_commands(material_module_legacy
    "find_package(lux-engine-editor-extensions REQUIRED COMPONENTS editor_extensions_legacy)"
    "find_package(lux-engine-editor-material-ui REQUIRED COMPONENTS material_ui_legacy)"
    "find_package(lux-engine-editor-material-persistence REQUIRED COMPONENTS material_persistence_legacy)")
lux_classify_target(TARGET material_module_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-material-module VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS material_module_legacy)
engine_declare_editor_module(TARGET material_module_legacy
    HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/MaterialModule.hpp"
    LOGICAL_PATH lux/engine/editor/material/MaterialModule.hpp
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialModule.cpp")
