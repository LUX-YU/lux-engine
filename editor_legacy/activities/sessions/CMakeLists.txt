add_component(COMPONENT_NAME session_factories_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/SessionInstallation.cpp src/SessionFactory.cpp src/SessionOperations.cpp src/SessionCommands.cpp)
component_include_directories(session_factories_legacy BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include)
target_link_libraries(session_factories_legacy PUBLIC lux::engine::editor::editor_commands_legacy lux::engine::editor::editor_persistence_legacy
    lux::engine::editor::edit_sessions_legacy lux::engine::resource::asset lux::cxx::core lux::engine::core::services)
component_add_transitive_commands(session_factories_legacy
    "find_package(lux-engine-editor-commands REQUIRED COMPONENTS editor_commands_legacy)"
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence_legacy)"
    "find_package(lux-engine-edit-sessions REQUIRED COMPONENTS edit_sessions_legacy)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-cxx REQUIRED COMPONENTS core)"
    "find_package(lux-engine-core REQUIRED COMPONENTS services)")
lux_classify_target(TARGET session_factories_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)

# Factory/installation and fixed-set operations remain independent of execution. These two concrete
# asynchronous activities form the single Process-facing boundary in the same installed package.
add_component(COMPONENT_NAME session_execution_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/SessionOpening.cpp src/ReloadSessionOperation.cpp src/SessionServices.cpp src/SessionHistoryCommands.cpp)
component_include_directories(session_execution_legacy BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include)
target_link_libraries(session_execution_legacy PUBLIC lux::engine::editor::session_factories_legacy
    lux::engine::process::process_execution)
component_add_transitive_commands(session_execution_legacy
    "find_package(lux-engine-editor-session-factories REQUIRED COMPONENTS session_factories_legacy)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)")
lux_classify_target(TARGET session_execution_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-session-factories VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS session_factories_legacy session_execution_legacy)

# One pure binding algorithm serves both explicit roles and declared async owners; never installed.
target_include_directories(session_factories_legacy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude)
target_include_directories(session_execution_legacy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/sinclude)
