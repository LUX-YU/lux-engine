find_package(imgui REQUIRED COMPONENTS core)
find_package(node_editor REQUIRED COMPONENTS node_editor)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_GUI_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_GUI_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_GUI_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/gui/visibility.h)
add_component(COMPONENT_NAME editor_gui NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_gui
    SOURCE_FILES src/shell/EditorWindow.cpp src/actions/ActiveEditHistory.cpp src/actions/HistoryActions.cpp
        src/asset/AssetReference.cpp
        src/GuiFrontend.cpp src/flowforge/FlowForgeDocumentProvider.cpp src/material/MaterialDocumentProvider.cpp src/scene/SceneDocumentProvider.cpp src/scene/SceneCamera.cpp src/scene/ScenePane.cpp
        src/scene/OutlinerPane.cpp src/scene/InspectorPane.cpp src/scene/ResourcePane.cpp
        src/FirstPartyComponentBindings.cpp src/CodegenInput.cpp)
set_property(TARGET editor_gui PROPERTY LUX_ARCH_LAYER EDITOR)
component_include_directories(editor_gui
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude INSTALL_TIME include)
target_compile_definitions(editor_gui PRIVATE LUX_EDITOR_GUI_LIBRARY)
target_link_libraries(editor_gui PUBLIC lux::engine::editor::editor_documents lux::engine::function::ui imgui::core
    PRIVATE imgui::node_editor lux::engine::editor::editor_ui lux::engine::platform::window lux::engine::simulation::transform_system
    lux::engine::scene::scene_render_meta lux::engine::function::render_client_meta)
component_add_transitive_commands(editor_gui
    "find_package(lux-engine-editor-documents REQUIRED COMPONENTS editor_documents)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package(imgui REQUIRED COMPONENTS core)")
if(WIN32)
    target_link_libraries(editor_gui PRIVATE imm32 comdlg32)
endif()
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_gui_history_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/gui/actions/HistoryActions.hpp
    EXTRA_COMPILE_OPTIONS -fdelayed-template-parsing
    REGISTER_FUNC_NAME lux_editor_gui_history)
engine_add_type_runtime_info(NAME editor_gui_window_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/gui/shell/EditorWindow.hpp
    EXTRA_COMPILE_OPTIONS -fdelayed-template-parsing
    REGISTER_FUNC_NAME lux_editor_gui_window)
engine_add_type_runtime_info(NAME editor_gui_transform_meta
    TARGET_FILES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/builtin/transform/include/lux/engine/simulation/TransformSystem.hpp
    REGISTER_FUNC_NAME lux_editor_gui_transform)
engine_target_add_type_runtime_info(TARGET editor_gui METAS editor_gui_history_meta editor_gui_transform_meta editor_gui_window_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_editor_imgui_inspector_codegen.cmake)
component_add_cmake_scripts(editor_gui
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_editor_imgui_inspector_codegen.cmake)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/codegen/
    DESTINATION share/lux-engine-editor-gui/editor_gui/cmake_scripts/codegen
    PATTERN "__pycache__" EXCLUDE)
engine_target_add_imgui_inspector_codegen(NAME desktop_transform TARGET editor_gui
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/transform/include/lux/engine/simulation/ecs/Transform.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Transform.hpp
    COMPONENTS lux::simulation::ecs::Transform2D lux::simulation::ecs::Transform3D)
engine_target_add_imgui_inspector_codegen(NAME desktop_hierarchy TARGET editor_gui
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/hierarchy/include/lux/engine/simulation/ecs/Parent.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Parent.hpp
    COMPONENTS lux::simulation::ecs::Parent
    CUSTOM_TYPES lux::simulation::ecs::Entity
    CUSTOM_HEADERS FirstPartyInspectorWidgets.hpp)
engine_target_add_imgui_inspector_codegen(NAME desktop_visual TARGET editor_gui
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/visual/include/lux/engine/simulation/ecs/Visual.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Visual.hpp
    COMPONENTS lux::simulation::ecs::Mesh3D lux::simulation::ecs::Light3D
    CUSTOM_TYPES lux::rdesc::MeshVisualDescription lux::rdesc::LightDescription
    CUSTOM_HEADERS FirstPartyInspectorWidgets.hpp)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-gui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_gui)
