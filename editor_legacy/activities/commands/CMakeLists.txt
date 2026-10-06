find_package(lux-cxx REQUIRED COMPONENTS core compile_time)
add_component(COMPONENT_NAME editor_commands_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/CommandRegistry.cpp src/CommandDispatcher.cpp)
component_include_directories(editor_commands_legacy BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_include_directories(editor_commands_legacy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)
target_link_libraries(editor_commands_legacy PUBLIC editor_contracts_legacy edit_sessions_legacy lux::cxx::core lux::cxx::compile_time
    lux::engine::function::ui_input lux::engine::core::services)
component_add_transitive_commands(editor_commands_legacy
    "find_package(lux-engine-core REQUIRED COMPONENTS services)"
    "find_package(lux-engine-function REQUIRED COMPONENTS ui_input)"
    "find_package(lux-engine-editor-contracts REQUIRED COMPONENTS editor_contracts_legacy)"
    "find_package(lux-engine-edit-sessions REQUIRED COMPONENTS edit_sessions_legacy)"
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time)")
lux_classify_target(TARGET editor_commands_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-commands VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_commands_legacy)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    target_compile_definitions(editor_commands_legacy PRIVATE LUX_COMMAND_TEST_ACCESS=1)
    add_executable(editor_commands_test_legacy test/commands.cpp)
    target_link_libraries(editor_commands_test_legacy PRIVATE editor_commands_legacy)
    lux_editor_test_options(editor_commands_test_legacy)
    add_test(NAME editor.commands COMMAND editor_commands_test_legacy)
    add_executable(editor_command_index_test_legacy test/index.cpp)
    target_link_libraries(editor_command_index_test_legacy PRIVATE editor_commands_legacy)
    target_include_directories(editor_command_index_test_legacy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude)
    lux_editor_test_options(editor_command_index_test_legacy)
    add_test(NAME editor.command_index COMMAND editor_command_index_test_legacy)
    set_tests_properties(editor.commands PROPERTIES LABELS "editor;native;P11")
endif()
