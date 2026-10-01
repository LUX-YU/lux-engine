find_package(imgui REQUIRED COMPONENTS core)
generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_UI_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_UI_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_UI_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/ui/visibility.h)
add_component(COMPONENT_NAME editor_ui NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_ui
    SOURCE_FILES src/TaskPane.cpp ../transition/LegacySceneConfiguration.cpp src/SpatialInteraction3D.cpp ../transition/LegacyComponentEditors.cpp ../transition/LegacyCodegenInput.cpp)
component_include_directories(editor_ui
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${PROJECT_SOURCE_DIR}/editor/transition ${CMAKE_CURRENT_SOURCE_DIR}/pinclude ${CMAKE_CURRENT_SOURCE_DIR}/sinclude ${PROJECT_SOURCE_DIR}/editor/assets/sinclude ${PROJECT_SOURCE_DIR}/editor/activities/project/sinclude ${PROJECT_SOURCE_DIR}/editor/activities/sinclude
    INSTALL_TIME include)
lux_classify_target(TARGET editor_ui LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
target_compile_definitions(editor_ui PRIVATE LUX_EDITOR_UI_LIBRARY)
target_link_libraries(editor_ui PUBLIC lux::engine::editor::project_ui lux::engine::editor::editor_widgets lux::engine::editor::tasks_ui lux::engine::editor::scene_ui lux::engine::editor::desktop_shell lux::engine::editor::editor_metadata lux::engine::editor::editor_editing_scene lux::engine::scene::scene_mesh_query
    lux::engine::editor::scene_projection
    PRIVATE lux::engine::editor::editor_context
    PUBLIC
    lux::engine::editor::editor_storage lux::engine::project::project_plugins
    lux::engine::function::ui lux::engine::function::ui_rendering lux::engine::scene::scene_render imgui::core)
component_add_transitive_commands(editor_ui
    "find_package(lux-engine-editor-project-ui REQUIRED COMPONENTS project_ui)"
    "find_package(lux-engine-editor-widgets REQUIRED COMPONENTS editor_widgets)"
    "find_package(lux-engine-editor-tasks-ui REQUIRED COMPONENTS tasks_ui)"
    "find_package(lux-engine-editor-scene-ui REQUIRED COMPONENTS scene_ui)"
    "find_package(lux-engine-editor-desktop REQUIRED COMPONENTS desktop_shell)"
    "find_package(lux-engine-editor-scene-projection REQUIRED COMPONENTS scene_projection)"
    "find_package(lux-engine-editor-context REQUIRED COMPONENTS editor_context)"
    "find_package(lux-engine-editor-metadata REQUIRED COMPONENTS editor_metadata)"
    "find_package(lux-engine-project-plugins REQUIRED COMPONENTS project_plugins)"
    "find_package(lux-engine-editor-editing-scene REQUIRED COMPONENTS editor_editing_scene)"
    "find_package(lux-engine-scene-mesh-query REQUIRED COMPONENTS scene_mesh_query)"
    "find_package(lux-engine-editor-storage REQUIRED COMPONENTS editor_storage)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui ui_rendering)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(imgui REQUIRED COMPONENTS core)")
include_component_cmake_scripts(meta)
include(${PROJECT_SOURCE_DIR}/editor/workbench/scene/cmake/engine_editor_imgui_inspector_codegen.cmake)
component_add_cmake_scripts(editor_ui
    ${PROJECT_SOURCE_DIR}/editor/workbench/scene/cmake/engine_editor_imgui_inspector_codegen.cmake)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/editor/workbench/scene/codegen/
    DESTINATION share/lux-engine-editor-ui/editor_ui/cmake_scripts/codegen
    PATTERN "__pycache__" EXCLUDE)
engine_target_add_imgui_inspector_codegen(NAME desktop_transform TARGET editor_ui
    SOURCE_FILE ${PROJECT_SOURCE_DIR}/editor/transition/LegacyCodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/transform/include/lux/engine/simulation/ecs/Transform.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Transform.hpp
    COMPONENTS lux::simulation::ecs::Transform2D lux::simulation::ecs::Transform3D)
engine_target_add_imgui_inspector_codegen(NAME desktop_hierarchy TARGET editor_ui
    SOURCE_FILE ${PROJECT_SOURCE_DIR}/editor/transition/LegacyCodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/hierarchy/include/lux/engine/simulation/ecs/Parent.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Parent.hpp
    COMPONENTS lux::simulation::ecs::Parent
    CUSTOM_TYPES lux::simulation::ecs::Entity
    CUSTOM_HEADERS FirstPartyInspectorControls.hpp)
engine_target_add_imgui_inspector_codegen(NAME desktop_visual TARGET editor_ui
    SOURCE_FILE ${PROJECT_SOURCE_DIR}/editor/transition/LegacyCodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/visual/include/lux/engine/simulation/ecs/Visual.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Visual.hpp
    COMPONENTS lux::simulation::ecs::Mesh3D lux::simulation::ecs::Light3D
    CUSTOM_TYPES lux::rdesc::LightDescription
    CUSTOM_ELEMENTS lux::rdesc::MeshVisualDescription=MeshVisualControl
    CUSTOM_HEADERS FirstPartyInspectorControls.hpp)
engine_target_add_imgui_inspector_codegen(NAME desktop_camera TARGET editor_ui
    SOURCE_FILE ${PROJECT_SOURCE_DIR}/editor/transition/LegacyCodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/scene/components/camera/include/lux/engine/scene/Camera.hpp
    LOGICAL_PATH lux/engine/scene/Camera.hpp
    COMPONENTS lux::scene::Camera)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_ui)


target_link_libraries(editor_ui PRIVATE lux::engine::platform::window lux::engine::function::input lux::engine::scene::scene_transform)

if(LUX_EDITOR_TEST_DESKTOP_GPU)
    add_executable(editor_ui_presentation test/unified_ui.cpp)
    target_link_libraries(editor_ui_presentation PRIVATE editor_ui lux::engine::platform::window
        lux::engine::function::render_features)
    if(MSVC)
        target_compile_options(editor_ui_presentation PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(editor_ui_presentation PRIVATE -UNDEBUG)
    endif()
endif()

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME editor.inspector_codegen COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/inspector_codegen.py")
    add_executable(editor_scene_configuration test/scene_configuration.cpp)
    target_link_libraries(editor_scene_configuration PRIVATE editor_ui lux::engine::scene::scene_asset)
    if(MSVC)
        target_compile_options(editor_scene_configuration PRIVATE /UNDEBUG /utf-8)
    else()
        target_compile_options(editor_scene_configuration PRIVATE -UNDEBUG)
    endif()
    add_test(NAME editor.scene_configuration COMMAND editor_scene_configuration "${CMAKE_BINARY_DIR}")
    set_tests_properties(editor.scene_configuration PROPERTIES TIMEOUT 30)
endif()

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    set_tests_properties(editor.inspector_codegen editor.scene_configuration
        PROPERTIES LABELS "editor;native")
endif()
