find_package(lux-cxx REQUIRED COMPONENTS compile_time)
generate_visibility_header(
    ENABLE_MACRO_NAME LUX_EDITOR_TOOLING_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_TOOLING_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_EDITOR_TOOLING_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/application/tooling/visibility.h)
add_component(COMPONENT_NAME editor_tooling NAMESPACE lux::engine::editor
    OUTPUT_NAME lux_engine_editor_tooling SOURCE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/Toolset.cpp)
component_include_directories(editor_tooling
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude INSTALL_TIME include)
target_compile_definitions(editor_tooling PRIVATE LUX_EDITOR_TOOLING_LIBRARY)
target_link_libraries(editor_tooling PUBLIC lux::cxx::compile_time)
component_add_transitive_commands(editor_tooling "find_package(lux-cxx REQUIRED COMPONENTS compile_time)")

install_components(PROJECT_NAME lux-engine-editor-tooling VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_tooling)
