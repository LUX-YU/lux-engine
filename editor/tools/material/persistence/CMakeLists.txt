add_component(COMPONENT_NAME material_persistence NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/MaterialSaveSource.cpp src/MaterialCodec.cpp)
component_include_directories(material_persistence
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(material_persistence PUBLIC lux::engine::editor::material_model lux::engine::editor::editor_persistence)
component_add_transitive_commands(material_persistence
    "find_package(lux-engine-editor-material-model REQUIRED COMPONENTS material_model)"
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence)")
lux_classify_target(TARGET material_persistence LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-material-persistence VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS material_persistence)
