add_component(COMPONENT_NAME layout_model_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/LayoutPlan.cpp src/WorkspaceCodec.cpp src/SettingsCodec.cpp)
component_include_directories(layout_model_legacy BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include)
target_link_libraries(layout_model_legacy PUBLIC lux::engine::editor::editor_contracts_legacy)
find_package(tomlplusplus CONFIG REQUIRED)
target_compile_definitions(layout_model_legacy PRIVATE TOML_HEADER_ONLY=1 TOML_EXCEPTIONS=0)
target_include_directories(layout_model_legacy PRIVATE $<TARGET_PROPERTY:tomlplusplus::tomlplusplus,INTERFACE_INCLUDE_DIRECTORIES>)
component_add_transitive_commands(layout_model_legacy "find_package(lux-engine-editor-contracts REQUIRED COMPONENTS editor_contracts_legacy)")
lux_classify_target(TARGET layout_model_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-layout VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS layout_model_legacy)
