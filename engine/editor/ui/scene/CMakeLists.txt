generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_SCENE_UI_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_SCENE_UI_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_SCENE_UI_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/ui/scene/visibility.h)
add_component(COMPONENT_NAME editor_scene_ui NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_scene_ui
    SOURCE_FILES src/SceneWorkspace.cpp src/SceneViewport.cpp src/SceneOutliner.cpp src/SceneInspector.cpp
        src/SceneResourcesPane.cpp src/FirstPartySceneReaders.cpp src/CodegenInput.cpp)
component_include_directories(editor_scene_ui
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude INSTALL_TIME include)
target_compile_definitions(editor_scene_ui PRIVATE LUX_EDITOR_SCENE_UI_LIBRARY)
target_link_libraries(editor_scene_ui PUBLIC lux::engine::editor::editor_ui lux::engine::editor::editor_scene_session
    PRIVATE lux::engine::simulation::ecs::hierarchy)
component_add_transitive_commands(editor_scene_ui
    "find_package(lux-engine-editor-ui REQUIRED COMPONENTS editor_ui)"
    "find_package(lux-engine-editor-scene-session REQUIRED COMPONENTS editor_scene_session)")
component_add_cmake_scripts(editor_scene_ui ${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_editor_scene_reader_codegen.cmake)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/template/
    DESTINATION share/lux-engine-editor-scene-ui/editor_scene_ui/cmake_scripts/template)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_editor_scene_reader_codegen.cmake)
engine_target_add_scene_reader_codegen(NAME scene_transform_readers TARGET editor_scene_ui
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/transform/include/lux/engine/simulation/ecs/Transform.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Transform.hpp SYMBOL transform)
engine_target_add_scene_reader_codegen(NAME scene_hierarchy_readers TARGET editor_scene_ui
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/hierarchy/include/lux/engine/simulation/ecs/Parent.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Parent.hpp SYMBOL hierarchy)
engine_target_add_scene_reader_codegen(NAME scene_visual_readers TARGET editor_scene_ui
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/visual/include/lux/engine/simulation/ecs/Visual.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Visual.hpp SYMBOL visual)

install_components(PROJECT_NAME lux-engine-editor-scene-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_scene_ui)
