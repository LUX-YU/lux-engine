# Composition is selectable independently of Scene/Material tools and the query-only browser.
add_component(COMPONENT_NAME project_module NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/ProjectModule.cpp)
component_include_directories(project_module BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(project_module PUBLIC lux::engine::editor::editor_extensions
    PRIVATE lux::engine::editor::project_ui lux::engine::editor::editor_storage lux::engine::editor::scene_project)
component_add_transitive_commands(project_module
    "find_package(lux-engine-editor-extensions REQUIRED COMPONENTS editor_extensions)"
    "find_package(lux-engine-editor-project-ui REQUIRED COMPONENTS project_ui)"
    "find_package(lux-engine-editor-storage REQUIRED COMPONENTS editor_storage)"
    "find_package(lux-engine-editor-scene-execution REQUIRED COMPONENTS scene_project)")
lux_classify_target(TARGET project_module LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-project-module VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS project_module)
engine_declare_editor_module(TARGET project_module
    HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/project/ProjectModule.hpp"
    LOGICAL_PATH lux/engine/editor/project/ProjectModule.hpp
    SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/ProjectModule.cpp")
