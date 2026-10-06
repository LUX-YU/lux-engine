add_component(COMPONENT_NAME editor_persistence_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/WriteCoordinator.cpp src/SaveService.cpp src/EncodeJob.cpp src/DerivedArtifact.cpp)
component_include_directories(editor_persistence_legacy
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(editor_persistence_legacy PUBLIC lux::engine::editor::edit_sessions_legacy lux::cxx::memory)
component_add_transitive_commands(editor_persistence_legacy
    "find_package(lux-engine-edit-sessions REQUIRED COMPONENTS edit_sessions_legacy)"
    "find_package(lux-cxx REQUIRED COMPONENTS memory)")
lux_classify_target(TARGET editor_persistence_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
add_component(COMPONENT_NAME editor_persistence_execution_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/SaveExecution.cpp src/PersistenceServices.cpp)
component_include_directories(editor_persistence_execution_legacy
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(editor_persistence_execution_legacy PUBLIC editor_persistence_legacy lux::engine::process::process_execution lux::engine::core::services)
component_add_transitive_commands(editor_persistence_execution_legacy
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence_legacy)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(lux-engine-core REQUIRED COMPONENTS services)")
lux_classify_target(TARGET editor_persistence_execution_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-persistence VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_persistence_legacy editor_persistence_execution_legacy)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_write_coordinator_test_legacy test/write_coordinator.cpp)
    target_link_libraries(editor_write_coordinator_test_legacy PRIVATE editor_persistence_legacy)
    lux_editor_test_options(editor_write_coordinator_test_legacy)
    add_test(NAME editor.persistence.write_coordinator COMMAND editor_write_coordinator_test_legacy)
    set_tests_properties(editor.persistence.write_coordinator PROPERTIES LABELS "editor;native;P05")
endif()

add_component(COMPONENT_NAME editor_file_publication_legacy NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/FilePublication.cpp src/FileArtifactStore.cpp src/PublicationFileStore.cpp)
component_include_directories(editor_file_publication_legacy
    BUILD_TIME_SHARED ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(editor_file_publication_legacy PUBLIC lux::cxx::compile_time lux::engine::editor::editor_persistence_legacy lux::engine::core::services PRIVATE lux::cxx::algorithm lux::engine::platform::platform_process)
component_add_transitive_commands(editor_file_publication_legacy
    "find_package(lux-engine-platform REQUIRED COMPONENTS platform_process)"
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time algorithm)"
    "find_package(lux-engine-editor-persistence REQUIRED COMPONENTS editor_persistence_legacy)"
    "find_package(lux-engine-core REQUIRED COMPONENTS services)")
lux_classify_target(TARGET editor_file_publication_legacy LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-file-publication VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_file_publication_legacy)


if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_publication_test_legacy test/publication.cpp)
    target_link_libraries(editor_publication_test_legacy PRIVATE editor_file_publication_legacy)
    lux_editor_test_options(editor_publication_test_legacy)
    add_test(NAME editor.persistence.real_files COMMAND editor_publication_test_legacy "${CMAKE_CURRENT_BINARY_DIR}/test-files")
    set_tests_properties(editor.persistence.real_files PROPERTIES LABELS "editor;native;P05")
endif()

# Exact SDK ownership: sibling providers share a build include root, never an install manifest.
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/FileArtifactStore.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/PublicationFileStore.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/storage/FilePublication.hpp
    DESTINATION include/lux/engine/editor/storage COMPONENT lux_engine_editor_file_publication)
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/persistence/ArtifactStore.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/persistence/DerivedArtifact.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/persistence/EncodeJob.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/persistence/SaveExecution.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/persistence/PersistenceServices.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/persistence/SaveService.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/persistence/SaveSource.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/persistence/SaveTypes.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/persistence/WriteCoordinator.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/editor/persistence/WriteLane.hpp
    DESTINATION include/lux/engine/editor/persistence COMPONENT lux_engine_editor_persistence)
