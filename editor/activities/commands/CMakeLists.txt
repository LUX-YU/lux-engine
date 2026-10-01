find_package(lux-cxx REQUIRED COMPONENTS core compile_time)
add_component(COMPONENT_NAME editor_commands NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/CommandRegistry.cpp src/CommandDispatcher.cpp)
component_include_directories(editor_commands BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(editor_commands PUBLIC editor_contracts edit_sessions lux::cxx::core lux::cxx::compile_time)
component_add_transitive_commands(editor_commands
    "find_package(lux-engine-editor-contracts REQUIRED COMPONENTS editor_contracts)"
    "find_package(lux-engine-edit-sessions REQUIRED COMPONENTS edit_sessions)"
    "find_package(lux-cxx REQUIRED COMPONENTS core compile_time)")
lux_classify_target(TARGET editor_commands LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-commands VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_commands)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_commands_test test/commands.cpp)
    target_link_libraries(editor_commands_test PRIVATE editor_commands)
    lux_editor_test_options(editor_commands_test)
    add_test(NAME editor.commands COMMAND editor_commands_test)
    set_tests_properties(editor.commands PROPERTIES LABELS "editor;native;P11")
endif()
