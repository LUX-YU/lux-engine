add_component(COMPONENT_NAME editor_persistence NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/WriteCoordinator.cpp src/SaveService.cpp src/EncodeJob.cpp)
component_include_directories(editor_persistence
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(editor_persistence PUBLIC lux::engine::editor::edit_sessions lux::cxx::memory)
component_add_transitive_commands(editor_persistence
    "find_package(lux-engine-edit-sessions REQUIRED COMPONENTS edit_sessions)"
    "find_package(lux-cxx REQUIRED COMPONENTS memory)")
lux_classify_target(TARGET editor_persistence LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
add_component(COMPONENT_NAME editor_persistence_execution NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/SaveExecution.cpp)
component_include_directories(editor_persistence_execution
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(editor_persistence_execution PUBLIC editor_persistence lux::engine::process::process_execution)
component_add_transitive_commands(editor_persistence_execution
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)")
lux_classify_target(TARGET editor_persistence_execution LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-persistence VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_persistence editor_persistence_execution)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_write_coordinator_test test/write_coordinator.cpp)
    target_link_libraries(editor_write_coordinator_test PRIVATE editor_persistence)
    lux_editor_test_options(editor_write_coordinator_test)
    add_test(NAME editor.persistence.write_coordinator COMMAND editor_write_coordinator_test)
    set_tests_properties(editor.persistence.write_coordinator PROPERTIES LABELS "editor;native;P05")
endif()

add_component(COMPONENT_NAME editor_file_publication NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/FilePublication.cpp src/FileArtifactStore.cpp)
component_include_directories(editor_file_publication
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(editor_file_publication PUBLIC lux::cxx::compile_time lux::engine::editor::editor_persistence PRIVATE lux::cxx::algorithm)
component_add_transitive_commands(editor_file_publication
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time algorithm)"
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence)")
lux_classify_target(TARGET editor_file_publication LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-file-publication VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_file_publication)


if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_publication_test test/publication.cpp)
    target_link_libraries(editor_publication_test PRIVATE editor_file_publication)
    lux_editor_test_options(editor_publication_test)
    add_test(NAME editor.persistence.real_files COMMAND editor_publication_test "${CMAKE_CURRENT_BINARY_DIR}/test-files")
    set_tests_properties(editor.persistence.real_files PROPERTIES LABELS "editor;native;P05")
endif()
