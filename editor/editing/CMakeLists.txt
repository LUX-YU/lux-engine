add_subdirectory(history)
add_subdirectory(sessions)

find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME LUX_EDITOR_EDITING_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_EDITING_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_EDITOR_EDITING_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/editing/visibility.h
)
add_component(
    COMPONENT_NAME editor_editing
    NAMESPACE lux::engine::editor
    OUTPUT_NAME lux_engine_editor_editing
    SOURCE_FILES src/EditHistoryTarget.cpp ../transition/LegacyPersistenceState.cpp
)
component_include_directories(
    editor_editing
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)
target_compile_definitions(editor_editing PRIVATE LUX_EDITOR_EDITING_LIBRARY)
lux_classify_target(TARGET editor_editing LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
target_link_libraries(editor_editing PUBLIC lux::cxx::compile_time lux::engine::resource::identity lux::engine::editor::edit_sessions)
component_add_transitive_commands(editor_editing "find_package(lux-engine-resource REQUIRED COMPONENTS identity)")
component_add_transitive_commands(editor_editing "find_package(lux-cxx REQUIRED COMPONENTS compile_time)")

component_add_transitive_commands(editor_editing "find_package(lux-engine-edit-sessions REQUIRED COMPONENTS edit_sessions)")

lux_engine_install_components(
    PROJECT_NAME lux-engine-editor-editing
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor
    COMPONENTS editor_editing
)

