add_component(COMPONENT_NAME editor_tasks NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/TaskMonitor.cpp)
component_include_directories(editor_tasks BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(editor_tasks PUBLIC lux::engine::core::object lux::engine::process::process_execution)
component_add_transitive_commands(editor_tasks
    "find_package(lux-engine-core REQUIRED COMPONENTS object)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)")
lux_classify_target(TARGET editor_tasks LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-tasks VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS editor_tasks)
if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_task_monitor_cpu_test test/monitor.cpp)
    target_link_libraries(editor_task_monitor_cpu_test PRIVATE editor_tasks)
    lux_editor_test_options(editor_task_monitor_cpu_test)
    add_test(NAME editor.tasks.monitor_cpu COMMAND editor_task_monitor_cpu_test)
    set_tests_properties(editor.tasks.monitor_cpu PROPERTIES LABELS "editor;native;editor_layering" TIMEOUT 60)
endif()
