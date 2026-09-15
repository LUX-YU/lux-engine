find_package(imgui REQUIRED COMPONENTS core)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_GUI_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_GUI_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_GUI_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/gui/visibility.h)
add_component(COMPONENT_NAME editor_gui NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_gui
    SOURCE_FILES src/shell/EditorWindow.cpp src/actions/ActiveEditHistory.cpp src/actions/HistoryActions.cpp
        src/GuiFrontend.cpp src/scene/SceneDocumentProvider.cpp src/scene/SceneCamera.cpp src/scene/ScenePane.cpp
        src/scene/OutlinerPane.cpp src/scene/InspectorPane.cpp src/scene/ResourcePane.cpp
        src/FirstPartySceneReaders.cpp src/CodegenInput.cpp)
component_include_directories(editor_gui
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude INSTALL_TIME include)
target_compile_definitions(editor_gui PRIVATE LUX_EDITOR_GUI_LIBRARY)
target_link_libraries(editor_gui PUBLIC lux::engine::editor::editor_documents lux::engine::function::ui
    PRIVATE imgui::core lux::engine::editor::editor_ui lux::engine::platform::window lux::engine::simulation::transform_system
    lux::engine::scene::scene_render_meta lux::engine::function::render_client_meta)
component_add_transitive_commands(editor_gui
    "find_package(lux-engine-editor-documents REQUIRED COMPONENTS editor_documents)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)")
if(WIN32)
    target_link_libraries(editor_gui PRIVATE imm32)
endif()
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_gui_history_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/gui/actions/HistoryActions.hpp
    EXTRA_COMPILE_OPTIONS -fdelayed-template-parsing
    REGISTER_FUNC_NAME lux_editor_gui_history)
engine_add_type_runtime_info(NAME editor_gui_transform_meta
    TARGET_FILES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/builtin/transform/include/lux/engine/simulation/TransformSystem.hpp
    REGISTER_FUNC_NAME lux_editor_gui_transform)
engine_target_add_type_runtime_info(TARGET editor_gui METAS editor_gui_history_meta editor_gui_transform_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_editor_scene_reader_codegen.cmake)
engine_target_add_scene_reader_codegen(NAME desktop_transform_readers TARGET editor_gui
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/transform/include/lux/engine/simulation/ecs/Transform.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Transform.hpp SYMBOL transform)
engine_target_add_scene_reader_codegen(NAME desktop_hierarchy_readers TARGET editor_gui
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/hierarchy/include/lux/engine/simulation/ecs/Parent.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Parent.hpp SYMBOL hierarchy)
engine_target_add_scene_reader_codegen(NAME desktop_visual_readers TARGET editor_gui
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/visual/include/lux/engine/simulation/ecs/Visual.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Visual.hpp SYMBOL visual)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-gui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_gui)
