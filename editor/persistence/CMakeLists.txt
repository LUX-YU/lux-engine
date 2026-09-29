add_component(COMPONENT_NAME editor_persistence NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/WriteCoordinator.cpp src/SaveService.cpp src/EncodeJob.cpp)
component_include_directories(editor_persistence
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(editor_persistence PUBLIC lux::engine::editor::edit_sessions)
component_add_transitive_commands(editor_persistence
    "find_package(lux-engine-edit-sessions REQUIRED COMPONENTS edit_sessions)")
lux_classify_target(TARGET editor_persistence LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-persistence VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_persistence)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_write_coordinator_test test/write_coordinator.cpp)
    target_link_libraries(editor_write_coordinator_test PRIVATE editor_persistence)
    lux_editor_test_options(editor_write_coordinator_test)
    add_test(NAME editor.persistence.write_coordinator COMMAND editor_write_coordinator_test)
    set_tests_properties(editor.persistence.write_coordinator PROPERTIES LABELS "editor;native;P05")
endif()
