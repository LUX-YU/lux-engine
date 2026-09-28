add_interface_component(COMPONENT_NAME editor_contracts NAMESPACE lux::engine::editor)
component_include_directories(editor_contracts
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
lux_classify_target(TARGET editor_contracts LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-contracts VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_contracts)
