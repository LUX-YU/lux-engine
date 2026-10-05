add_subdirectory(api)
find_package(imgui REQUIRED COMPONENTS core)
add_component(COMPONENT_NAME scene_ui NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/ConfigurationEditor.cpp src/SceneCreationPoint.cpp src/SceneView.cpp src/SceneTools.cpp src/SceneCommands.cpp src/OutlinerView.cpp src/ResourceView.cpp src/InspectorFields.cpp src/InspectorView.cpp src/RunInspectorFields.cpp src/RunInspectorView.cpp src/ComponentEditors.cpp src/CodegenInput.cpp src/SceneConfigurationElement.cpp src/SceneCreationView.cpp src/SceneConfigurationView.cpp)
component_include_directories(scene_ui BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_include_directories(scene_ui PRIVATE ${PROJECT_SOURCE_DIR}/editor/workbench/sinclude)
target_link_libraries(scene_ui PUBLIC lux::engine::editor::editor_composition lux::engine::editor::session_factories lux::engine::editor::scene_control_api lux::engine::editor::scene_execution lux::engine::editor::editor_configuration lux::engine::editor::editor_viewport lux::engine::editor::view_api lux::engine::editor::scene_projection
    lux::engine::editor::scene_interaction lux::engine::editor::project_ui lux::engine::editor::editor_widgets lux::engine::scene::scene_mesh_query PUBLIC lux::engine::project::project_plugins PRIVATE lux::engine::editor::scene_persistence lux::engine::scene::scene_transform lux::engine::editor::view_host imgui::core)
component_add_transitive_commands(scene_ui
    "find_package(lux-engine-editor-composition REQUIRED COMPONENTS editor_composition)"
    "find_package(lux-engine-editor-commands REQUIRED COMPONENTS editor_commands)"
    "find_package(lux-engine-editor-session-factories REQUIRED COMPONENTS session_factories)"
    "find_package(lux-engine-editor-scene-persistence REQUIRED COMPONENTS scene_persistence)"
    "find_package(lux-engine-editor-scene-controls REQUIRED COMPONENTS scene_control_api)"
    "find_package(lux-engine-editor-scene-execution REQUIRED COMPONENTS scene_execution)"
    "find_package(lux-engine-editor-configuration REQUIRED COMPONENTS editor_configuration)"
    "find_package(lux-engine-editor-viewport REQUIRED COMPONENTS editor_viewport)"
    "find_package(lux-engine-editor-project-ui REQUIRED COMPONENTS project_ui)"
    "find_package(lux-engine-editor-widgets REQUIRED COMPONENTS editor_widgets)"
    "find_package(lux-engine-editor-view-api REQUIRED COMPONENTS view_api)"
    "find_package(lux-engine-editor-scene-projection REQUIRED COMPONENTS scene_projection)"
    "find_package(lux-engine-editor-scene-interaction REQUIRED COMPONENTS scene_interaction)"
    "find_package(lux-engine-scene-mesh-query REQUIRED COMPONENTS scene_mesh_query)"
    "find_package(lux-engine-project-plugins REQUIRED COMPONENTS project_plugins)"
    "find_package(lux-engine-scene-transform REQUIRED COMPONENTS scene_transform)"
    "find_package(lux-engine-editor-desktop REQUIRED COMPONENTS view_host)"
    "find_package(imgui REQUIRED COMPONENTS core)")
lux_classify_target(TARGET scene_ui LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
target_link_libraries(scene_ui PUBLIC lux::engine::editor::editor_commands)

# Host-only semantic/template tool. It consumes the original MetaUnit and never links UI/runtime libraries.
if(CMAKE_CROSSCOMPILING)
    if(NOT LUX_INSPECTOR_GENERATOR_EXECUTABLE)
        message(FATAL_ERROR "Cross builds require an explicit host LUX_INSPECTOR_GENERATOR_EXECUTABLE")
    endif()
else()
    find_package(inja CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
    add_executable(lux_inspector_generator codegen/InspectorGenerator.cpp codegen/InspectorModel.cpp)
    target_link_libraries(lux_inspector_generator PRIVATE lux::cxx::reflection_runtime pantor::inja nlohmann_json::nlohmann_json)
    lux_classify_target(TARGET lux_inspector_generator LAYER EDITOR PRODUCT TOOLCHAIN ROLE GENERATOR)
    if(WIN32)
        add_custom_command(TARGET lux_inspector_generator POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:lux_inspector_generator>
                $<TARGET_FILE_DIR:lux_inspector_generator> COMMAND_EXPAND_LISTS VERBATIM)
    endif()
    install(TARGETS lux_inspector_generator RUNTIME DESTINATION bin COMPONENT lux_editor)
    install(FILES $<TARGET_RUNTIME_DLLS:lux_inspector_generator> DESTINATION bin COMPONENT lux_editor)
endif()

include_component_cmake_scripts(meta)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_editor_imgui_inspector_codegen.cmake)
engine_target_add_imgui_inspector_codegen(NAME scene_fields_transform TARGET scene_ui
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/transform/include/lux/engine/simulation/ecs/Transform.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Transform.hpp
    COMPONENTS lux::simulation::ecs::Transform2D lux::simulation::ecs::Transform3D)
engine_target_add_imgui_inspector_codegen(NAME scene_fields_hierarchy TARGET scene_ui
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/hierarchy/include/lux/engine/simulation/ecs/Parent.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Parent.hpp
    COMPONENTS lux::simulation::ecs::Parent
    CUSTOM_TYPES lux::simulation::ecs::Entity
    CUSTOM_HEADERS FirstPartyInspectorControls.hpp)
engine_target_add_imgui_inspector_codegen(NAME scene_fields_visual TARGET scene_ui
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/visual/include/lux/engine/simulation/ecs/Visual.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Visual.hpp
    COMPONENTS lux::simulation::ecs::Mesh3D lux::simulation::ecs::Light3D
    CUSTOM_TYPES lux::rdesc::LightDescription
    CUSTOM_ELEMENTS lux::rdesc::MeshVisualDescription=MeshVisualControl
    CUSTOM_HEADERS FirstPartyInspectorControls.hpp)
engine_target_add_imgui_inspector_codegen(NAME scene_fields_camera TARGET scene_ui
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/scene/components/camera/include/lux/engine/scene/Camera.hpp
    LOGICAL_PATH lux/engine/scene/Camera.hpp
    COMPONENTS lux::scene::Camera)

component_add_cmake_scripts(scene_ui ${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_editor_imgui_inspector_codegen.cmake)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/codegen/templates ${CMAKE_CURRENT_SOURCE_DIR}/codegen/support
    DESTINATION share/lux-engine-editor-scene-ui/scene_ui/cmake_scripts/codegen)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/codegen/ir.template ${CMAKE_CURRENT_SOURCE_DIR}/codegen/GenerateInspector.cmake
    DESTINATION share/lux-engine-editor-scene-ui/scene_ui/cmake_scripts/codegen)
install(FILES ${PROJECT_SOURCE_DIR}/.clang-format DESTINATION share/lux-engine-editor-scene-ui/scene_ui/cmake_scripts/codegen)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-scene-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS scene_ui)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME editor.inspector_codegen COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test/inspector_codegen.py"
        "$<TARGET_FILE:lux_inspector_generator>" "${CMAKE_CURRENT_SOURCE_DIR}/codegen/templates")
    set_tests_properties(editor.inspector_codegen PROPERTIES LABELS "editor;native")
endif()
