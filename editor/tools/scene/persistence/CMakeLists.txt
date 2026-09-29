add_component(COMPONENT_NAME scene_persistence NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/SceneSaveSource.cpp src/SceneCodec.cpp)
component_include_directories(scene_persistence
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(scene_persistence PUBLIC lux::engine::editor::scene_model lux::engine::editor::editor_persistence)
component_add_transitive_commands(scene_persistence
    "find_package(lux-engine-editor-scene-model REQUIRED COMPONENTS scene_model)"
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence)")
lux_classify_target(TARGET scene_persistence LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-scene-persistence VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS scene_persistence)
