generate_visibility_header(ENABLE_MACRO_NAME LUX_EDITOR_UI_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_UI_PUBLIC DISABLE_DLL_MACRO_NAME LUX_EDITOR_UI_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/ui/visibility.h)
add_component(COMPONENT_NAME editor_ui NAMESPACE lux::engine::editor OUTPUT_NAME lux_engine_editor_ui
    SOURCE_FILES src/SceneLayout.cpp)
component_include_directories(editor_ui
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR} INSTALL_TIME include)
target_compile_definitions(editor_ui PRIVATE LUX_EDITOR_UI_LIBRARY)
target_link_libraries(editor_ui PUBLIC lux::engine::function::ui)
component_add_transitive_commands(editor_ui "find_package(lux-engine-function REQUIRED COMPONENTS ui)")
lux_engine_install_components(PROJECT_NAME lux-engine-editor-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_ui)
