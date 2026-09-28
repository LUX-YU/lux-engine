find_package(lux-cxx REQUIRED COMPONENTS compile_time)

generate_visibility_header(
    ENABLE_MACRO_NAME LUX_EDIT_HISTORY_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDIT_HISTORY_PUBLIC
    DISABLE_DLL_MACRO_NAME LUX_EDIT_HISTORY_DLL_DISABLE
    GENERATE_FILE_PATH lux/engine/editor/editing/history_visibility.h
)
add_component(
    COMPONENT_NAME edit_history
    NAMESPACE lux::engine::editor
    OUTPUT_NAME lux_engine_edit_history
    SOURCE_FILES src/EditTypes.cpp src/EditHistory.cpp
)
component_include_directories(
    edit_history
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)
target_compile_definitions(edit_history PRIVATE LUX_EDIT_HISTORY_LIBRARY)
lux_classify_target(TARGET edit_history LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
target_link_libraries(edit_history PUBLIC lux::cxx::compile_time lux::engine::editor::editor_contracts)
component_add_transitive_commands(edit_history "find_package(lux-engine-editor-contracts REQUIRED COMPONENTS editor_contracts)")
component_add_transitive_commands(edit_history "find_package(lux-cxx REQUIRED COMPONENTS compile_time)")

lux_engine_install_components(
    PROJECT_NAME lux-engine-edit-history
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor
    COMPONENTS edit_history
)
