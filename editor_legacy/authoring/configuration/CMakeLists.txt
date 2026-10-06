generate_visibility_header(
    ENABLE_MACRO_NAME LUX_EDITOR_CONFIGURATION_LIBRARY
    PUBLIC_MACRO_NAME LUX_EDITOR_CONFIGURATION_PUBLIC
    GENERATE_FILE_PATH lux/engine/editor/configuration/visibility.h)
# One reflection lifetime state shared by application and Editor extension DSOs.
add_component(COMPONENT_NAME editor_configuration_legacy NAMESPACE lux::engine::editor SHARED
    SOURCE_FILES src/ConfigurationValue.cpp src/EditorReflection.cpp src/Settings.cpp)
component_include_directories(editor_configuration_legacy BUILD_TIME_EXPORT
    ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR} INSTALL_TIME include)
target_compile_definitions(editor_configuration_legacy PRIVATE LUX_EDITOR_CONFIGURATION_LIBRARY)
target_link_libraries(editor_configuration_legacy PUBLIC lux::engine::core::meta lux::engine::core::serialization
    lux::engine::core::object lux::engine::editor::editor_contracts_legacy lux::engine::editor::layout_model_legacy)
component_add_transitive_commands(editor_configuration_legacy
    "find_package(lux-engine-core REQUIRED COMPONENTS meta serialization object)"
    "find_package(lux-engine-editor-contracts REQUIRED COMPONENTS editor_contracts_legacy)"
    "find_package(lux-engine-editor-layout REQUIRED COMPONENTS layout_model_legacy)")
lux_classify_target(TARGET editor_configuration_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-configuration VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_configuration_legacy)
