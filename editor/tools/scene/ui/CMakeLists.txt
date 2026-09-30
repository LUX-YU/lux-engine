find_package(imgui REQUIRED COMPONENTS core)
add_component(COMPONENT_NAME scene_ui NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/CameraNavigation.cpp src/SceneElement.cpp src/SceneView.cpp src/ViewFactory.cpp src/OutlinerView.cpp src/ResourceView.cpp src/InspectorFields.cpp src/InspectorView.cpp src/ComponentEditors.cpp src/CodegenInput.cpp src/SceneConfigurationElement.cpp src/SceneCreationView.cpp src/SceneConfigurationView.cpp)
component_include_directories(scene_ui BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(scene_ui PUBLIC lux::engine::editor::view_api lux::engine::editor::scene_projection
    lux::engine::editor::scene_interaction lux::engine::editor::project_ui lux::engine::editor::editor_widgets lux::engine::scene::scene_mesh_query PUBLIC lux::engine::project::project_plugins PRIVATE lux::engine::scene::scene_transform imgui::core)
component_add_transitive_commands(scene_ui
    "find_package(lux-engine-editor-project-ui REQUIRED COMPONENTS project_ui)"
    "find_package(lux-engine-editor-widgets REQUIRED COMPONENTS editor_widgets)"
    "find_package(lux-engine-editor-view-api REQUIRED COMPONENTS view_api)"
    "find_package(lux-engine-editor-scene-projection REQUIRED COMPONENTS scene_projection)"
    "find_package(lux-engine-editor-scene-interaction REQUIRED COMPONENTS scene_interaction)"
    "find_package(lux-engine-scene-mesh-query REQUIRED COMPONENTS scene_mesh_query)"
    "find_package(lux-engine-project-plugins REQUIRED COMPONENTS project_plugins)"
    "find_package(lux-engine-scene-transform REQUIRED COMPONENTS scene_transform)"
    "find_package(imgui REQUIRED COMPONENTS core)")
lux_classify_target(TARGET scene_ui LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_scene_views_test test/views.cpp)
    target_link_libraries(editor_scene_views_test PRIVATE scene_ui desktop_shell material_ui flow_ui scene_execution tasks_ui project_io material_persistence
        lux::engine::scene::scene_transform lux::engine::function::ui_rendering
        lux::engine::function::render_features imgui::core)
    lux_editor_test_options(editor_scene_views_test)
    find_program(P10_LINKER NAMES lld-link REQUIRED)
    add_test(NAME editor.scene_views_gpu COMMAND editor_scene_views_test "${P10_LINKER}" "${CMAKE_BINARY_DIR}")
    if(WIN32)
        target_link_libraries(editor_scene_views_test PRIVATE user32)
        add_test(NAME editor.desktop_native_input COMMAND editor_scene_views_test "${P10_LINKER}" "${CMAKE_BINARY_DIR}" native)
        set_tests_properties(editor.desktop_native_input PROPERTIES LABELS "editor;P10;desktop;gpu" TIMEOUT 120 RUN_SERIAL TRUE)
    endif()
    set_tests_properties(editor.scene_views_gpu PROPERTIES LABELS "editor;P10;gpu" TIMEOUT 120)
endif()

include_component_cmake_scripts(meta)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_editor_imgui_inspector_codegen.cmake)
engine_target_add_imgui_inspector_codegen(NAME scene_fields_transform TARGET scene_ui SESSION_FIELDS
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/transform/include/lux/engine/simulation/ecs/Transform.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Transform.hpp
    COMPONENTS lux::simulation::ecs::Transform2D lux::simulation::ecs::Transform3D)
engine_target_add_imgui_inspector_codegen(NAME scene_fields_hierarchy TARGET scene_ui SESSION_FIELDS
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/hierarchy/include/lux/engine/simulation/ecs/Parent.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Parent.hpp
    COMPONENTS lux::simulation::ecs::Parent
    CUSTOM_TYPES lux::simulation::ecs::Entity
    CUSTOM_HEADERS FirstPartyInspectorControls.hpp)
engine_target_add_imgui_inspector_codegen(NAME scene_fields_visual TARGET scene_ui SESSION_FIELDS
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/visual/include/lux/engine/simulation/ecs/Visual.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Visual.hpp
    COMPONENTS lux::simulation::ecs::Mesh3D lux::simulation::ecs::Light3D
    CUSTOM_TYPES lux::rdesc::LightDescription
    CUSTOM_ELEMENTS lux::rdesc::MeshVisualDescription=MeshVisualControl
    CUSTOM_HEADERS FirstPartyInspectorControls.hpp)
engine_target_add_imgui_inspector_codegen(NAME scene_fields_camera TARGET scene_ui SESSION_FIELDS
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/scene/components/camera/include/lux/engine/scene/Camera.hpp
    LOGICAL_PATH lux/engine/scene/Camera.hpp
    COMPONENTS lux::scene::Camera)

component_add_cmake_scripts(scene_ui ${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_editor_imgui_inspector_codegen.cmake)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/codegen/ DESTINATION share/lux-engine-editor-scene-ui/scene_ui/cmake_scripts/codegen PATTERN "__pycache__" EXCLUDE)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-scene-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS scene_ui)
