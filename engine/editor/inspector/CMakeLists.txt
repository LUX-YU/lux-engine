find_package(Eigen3 CONFIG REQUIRED)
find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_EDITOR_INSPECTOR_LIBRARY
    PUBLIC_MACRO_NAME       LUX_EDITOR_INSPECTOR_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_EDITOR_INSPECTOR_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/editor/inspector/visibility.h
)

add_component(
    COMPONENT_NAME editor_inspector
    NAMESPACE      lux::engine::editor
    OUTPUT_NAME    lux_engine_editor_inspector
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ComponentEditorBinding.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/EntityInspector.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/FirstPartyComponentEditors.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/InspectorUndoJournal.cpp
)

component_include_directories(
    editor_inspector
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/component_editor_gen
    INSTALL_TIME include
)

target_compile_definitions(editor_inspector PRIVATE LUX_EDITOR_INSPECTOR_LIBRARY)
target_link_libraries(
    editor_inspector
    PUBLIC
        Eigen3::Eigen
        lux::cxx::compile_time
        lux::engine::editor::editor_context
        lux::engine::function::ui
        lux::engine::resource::asset
        lux::engine::resource::description
        lux::engine::simulation::ecs::schema
        lux::engine::simulation::ecs::transform
        lux::engine::simulation::ecs::hierarchy
        lux::engine::simulation::ecs::visual
        lux::engine::scene::scene_composition
)
component_add_transitive_commands(
    editor_inspector
    "find_package(Eigen3 CONFIG REQUIRED)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "find_package(lux-engine-editor-context REQUIRED COMPONENTS editor_context)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset description)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS schema transform hierarchy visual)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
)

component_add_cmake_scripts(
    editor_inspector
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_editor_inspector_codegen.cmake
)
install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/template/
    DESTINATION share/lux-engine-editor-inspector/editor_inspector/cmake_scripts/template
)

include_component_cmake_scripts(meta)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/engine_editor_inspector_codegen.cmake)
engine_target_add_component_editor_codegen(
    NAME transform_component_editor_codegen
    TARGET editor_inspector
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/transform/include/lux/engine/simulation/ecs/Transform.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Transform.hpp
    SYMBOL transform
)
engine_target_add_component_editor_codegen(
    NAME hierarchy_component_editor_codegen
    TARGET editor_inspector
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/hierarchy/include/lux/engine/simulation/ecs/Parent.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Parent.hpp
    SYMBOL hierarchy
)
engine_target_add_component_editor_codegen(
    NAME visual_component_editor_codegen
    TARGET editor_inspector
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CodegenInput.cpp
    HEADER ${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/visual/include/lux/engine/simulation/ecs/Visual.hpp
    LOGICAL_PATH lux/engine/simulation/ecs/Visual.hpp
    SYMBOL visual
)

lux_classify_target(
    TARGET  editor_inspector
    LAYER   EDITOR
    PRODUCT EDITOR
    ROLE    INTEGRATION
)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()

install_components(
    PROJECT_NAME lux-engine-editor-inspector
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::editor
    COMPONENTS   editor_inspector
)
