add_component(COMPONENT_NAME editor_context NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/EditorContext.cpp src/PaneManager.cpp)
lux_classify_target(TARGET editor_context LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
component_include_directories(editor_context
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude ${PROJECT_SOURCE_DIR}/editor/assets/sinclude ${PROJECT_SOURCE_DIR}/editor/storage/sinclude ${PROJECT_SOURCE_DIR}/editor/editing/sinclude)
target_link_libraries(editor_context PUBLIC lux::engine::engine_context_render lux::engine::editor::editor_assets
    lux::engine::editor::editor_metadata lux::engine::core::object lux::engine::function::ui
    lux::engine::scene::scene_render)
component_add_transitive_commands(editor_context
    "find_package(lux-engine-core REQUIRED COMPONENTS object)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package(lux-engine-context-render REQUIRED COMPONENTS engine_context_render)"
    "find_package(lux-engine-editor-assets REQUIRED COMPONENTS editor_assets)"
    "find_package(lux-engine-editor-metadata REQUIRED COMPONENTS editor_metadata)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)")
lux_engine_install_components(PROJECT_NAME lux-engine-editor-context VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_context)
