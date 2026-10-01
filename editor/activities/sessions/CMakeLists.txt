add_component(COMPONENT_NAME session_factories NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/SessionInstallation.cpp src/SessionFactory.cpp)
component_include_directories(session_factories BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include)
target_link_libraries(session_factories PUBLIC lux::engine::editor::editor_persistence
    lux::engine::editor::edit_sessions lux::engine::resource::asset lux::cxx::core)
component_add_transitive_commands(session_factories
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence)"
    "find_package(lux-engine-edit-sessions REQUIRED COMPONENTS edit_sessions)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-cxx REQUIRED COMPONENTS core)")
lux_classify_target(TARGET session_factories LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-session-factories VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS session_factories)

