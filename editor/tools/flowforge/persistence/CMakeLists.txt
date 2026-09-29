add_component(COMPONENT_NAME flowforge_persistence NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/FlowSaveSource.cpp src/FlowCodec.cpp)
component_include_directories(flowforge_persistence
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(flowforge_persistence PUBLIC lux::engine::editor::flowforge_model lux::engine::editor::editor_persistence)
component_add_transitive_commands(flowforge_persistence
    "find_package(lux-engine-editor-flowforge-model REQUIRED COMPONENTS flowforge_model)"
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence)")
lux_classify_target(TARGET flowforge_persistence LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-flowforge-persistence VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS flowforge_persistence)
