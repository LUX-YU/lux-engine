generate_visibility_header(
    ENABLE_MACRO_NAME LUX_EDITOR_SCENE_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_SCENE_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_EDITOR_SCENE_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/scene/visibility.h
)
add_component(COMPONENT_NAME editor_scene NAMESPACE lux::engine::editor
    OUTPUT_NAME lux_engine_editor_scene
    SOURCE_FILES src/NativeScene.cpp src/SceneCapture.cpp src/SceneEditor.cpp src/SceneObjects.cpp src/SceneObjectEdits.cpp src/SceneOpening.cpp src/SceneResources.cpp src/SceneRun.cpp
)
component_include_directories(editor_scene
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${PROJECT_SOURCE_DIR}/engine/editor/core/sinclude
    INSTALL_TIME include
)
target_compile_definitions(editor_scene PRIVATE LUX_EDITOR_SCENE_LIBRARY)
target_link_libraries(editor_scene PUBLIC
    lux::engine::editor::editor_core lux::engine::editor::editor_rendering
    lux::engine::scene::scene_composition lux::engine::scene::scene_meta
    lux::engine::scene::scene_world_materialization lux::engine::process::world_loading
    lux::engine::scene::scene_asset lux::engine::world::world_asset lux::engine::simulation::simulation_asset
    lux::engine::simulation::ecs::hierarchy
    PRIVATE lux::cxx::concurrent lux::engine::simulation::transform_system
        lux::engine::scene::scene_render_meta lux::engine::function::render_client_meta
)
component_add_transitive_commands(editor_scene
    "find_package(lux-engine-editor-core REQUIRED COMPONENTS editor_core)"
    "find_package(lux-engine-editor-rendering REQUIRED COMPONENTS editor_rendering)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-scene-meta REQUIRED COMPONENTS scene_meta)"
    "find_package(lux-engine-scene-world-materialization REQUIRED COMPONENTS scene_world_materialization)"
    "find_package(lux-engine-process-world-loading REQUIRED COMPONENTS process_world_loading)"
    "find_package(lux-engine-scene-asset REQUIRED COMPONENTS scene_asset)"
    "find_package(lux-engine-world-asset REQUIRED COMPONENTS world_asset)"
    "find_package(lux-engine-simulation-asset REQUIRED COMPONENTS simulation_asset)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS hierarchy)"
)
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_scene_document_meta
    TARGET_FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/scene/SceneEditor.hpp
    EXTRA_COMPILE_OPTIONS -fdelayed-template-parsing
    REGISTER_FUNC_NAME lux_editor_scene_document)
engine_target_add_type_runtime_info(TARGET editor_scene METAS editor_scene_document_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)

find_package(imgui REQUIRED COMPONENTS core)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_SCENE_UI_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_SCENE_UI_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_SCENE_UI_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/scene/ui/visibility.h)
add_component(COMPONENT_NAME editor_scene_ui NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_scene_ui
    SOURCE_FILES src/ui/SceneDocumentProvider.cpp
        src/ui/SceneLayout.cpp src/ui/SceneCamera.cpp src/ui/ScenePane.cpp
        src/ui/OutlinerPane.cpp src/ui/InspectorPane.cpp src/ui/ResourcePane.cpp
        src/ui/FirstPartyComponentBindings.cpp src/ui/CodegenInput.cpp)
set_property(TARGET editor_scene_ui PROPERTY LUX_ARCH_LAYER EDITOR)
component_include_directories(editor_scene_ui
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${PROJECT_SOURCE_DIR}/engine/editor/ui/sinclude INSTALL_TIME include)
target_compile_definitions(editor_scene_ui PRIVATE LUX_EDITOR_SCENE_UI_LIBRARY)
option(LUX_EDITOR_UI_DIAGNOSTICS "Private UI work counters; exclude from normal SDK and timing" OFF)
if(LUX_EDITOR_UI_DIAGNOSTICS)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        message(FATAL_ERROR "Editor UI diagnostics require RelWithDebInfo")
    endif()
    target_compile_definitions(editor_scene_ui PRIVATE LUX_EDITOR_UI_DIAGNOSTICS)
endif()
target_link_libraries(editor_scene_ui PUBLIC lux::engine::editor::editor_scene lux::engine::editor::editor_ui lux::engine::editor::editor_assets lux::engine::function::ui imgui::core
    PRIVATE lux::engine::editor::editor_ui lux::engine::platform::window lux::engine::simulation::transform_system
    lux::engine::scene::scene_render_meta lux::engine::function::render_client_meta)
component_add_transitive_commands(editor_scene_ui
    "find_package(lux-engine-editor-ui REQUIRED COMPONENTS editor_ui)"
    "find_package(lux-engine-editor-assets REQUIRED COMPONENTS editor_assets)"
    "find_package(lux-engine-editor-scene REQUIRED COMPONENTS editor_scene)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package(imgui REQUIRED COMPONENTS core)")
if(WIN32)
    target_link_libraries(editor_scene_ui PRIVATE imm32 comdlg32)
endif()
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_scene_transform_meta
    TARGET_FILES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/builtin_systems/transform/include/lux/engine/simulation/TransformSystem.hpp
    REGISTER_FUNC_NAME lux_editor_scene_transform)
engine_target_add_type_runtime_info(TARGET editor_scene METAS editor_scene_transform_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_editor_imgui_inspector_codegen.cmake)
component_add_cmake_scripts(editor_scene_ui
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_editor_imgui_inspector_codegen.cmake)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/codegen/
    DESTINATION share/lux-engine-editor-scene/editor_scene_ui/cmake_scripts/codegen
    PATTERN "__pycache__" EXCLUDE)
engine_target_add_imgui_inspector_codegen(NAME desktop_transform TARGET editor_scene_ui
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/transform/include/lux/engine/simulation/ecs/Transform.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Transform.hpp
    COMPONENTS lux::simulation::ecs::Transform2D lux::simulation::ecs::Transform3D)
engine_target_add_imgui_inspector_codegen(NAME desktop_hierarchy TARGET editor_scene_ui
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/hierarchy/include/lux/engine/simulation/ecs/Parent.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Parent.hpp
    COMPONENTS lux::simulation::ecs::Parent
    CUSTOM_TYPES lux::simulation::ecs::Entity
    CUSTOM_HEADERS FirstPartyInspectorWidgets.hpp)
engine_target_add_imgui_inspector_codegen(NAME desktop_visual TARGET editor_scene_ui
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/visual/include/lux/engine/simulation/ecs/Visual.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Visual.hpp
    COMPONENTS lux::simulation::ecs::Mesh3D lux::simulation::ecs::Light3D
    CUSTOM_TYPES lux::rdesc::MeshVisualDescription lux::rdesc::LightDescription
    CUSTOM_HEADERS FirstPartyInspectorWidgets.hpp)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-scene VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_scene editor_scene_ui)
