# Product/plugin composition owns declarations; ordinary UI consumers do not import the plugin protocol.
add_component(COMPONENT_NAME scene_module NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/SceneModule.cpp)
component_include_directories(scene_module BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(scene_module PUBLIC lux::engine::editor::editor_extensions
    PRIVATE lux::engine::editor::scene_ui lux::engine::editor::scene_persistence lux::engine::editor::scene_project)
component_add_transitive_commands(scene_module
    "find_package(lux-engine-editor-extensions REQUIRED COMPONENTS editor_extensions)"
    "find_package(lux-engine-editor-scene-ui REQUIRED COMPONENTS scene_ui)"
    "find_package(lux-engine-editor-scene-persistence REQUIRED COMPONENTS scene_persistence)"
    "find_package(lux-engine-editor-scene-execution REQUIRED COMPONENTS scene_project)")
lux_classify_target(TARGET scene_module LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-scene-module VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS scene_module)
engine_declare_editor_module(TARGET scene_module
    HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/SceneModule.hpp"
    LOGICAL_PATH lux/engine/editor/scene/SceneModule.hpp
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/SceneModule.cpp")

