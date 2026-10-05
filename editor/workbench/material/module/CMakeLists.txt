# Product/plugin composition owns declarations; ordinary UI consumers do not import the plugin protocol.
add_component(COMPONENT_NAME material_module NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/MaterialModule.cpp)
component_include_directories(material_module BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(material_module PUBLIC lux::engine::editor::editor_extensions
    PRIVATE lux::engine::editor::material_ui lux::engine::editor::material_persistence)
component_add_transitive_commands(material_module
    "find_package(lux-engine-editor-extensions REQUIRED COMPONENTS editor_extensions)"
    "find_package(lux-engine-editor-material-ui REQUIRED COMPONENTS material_ui)"
    "find_package(lux-engine-editor-material-persistence REQUIRED COMPONENTS material_persistence)")
lux_classify_target(TARGET material_module LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-material-module VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS material_module)
engine_declare_editor_module(TARGET material_module
    HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/material/MaterialModule.hpp"
    LOGICAL_PATH lux/engine/editor/material/MaterialModule.hpp
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/MaterialModule.cpp")
