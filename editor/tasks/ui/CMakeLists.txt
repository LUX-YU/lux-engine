find_package(imgui REQUIRED COMPONENTS core)
add_component(COMPONENT_NAME tasks_ui NAMESPACE lux::engine::editor STATIC SOURCE_FILES src/TaskView.cpp src/TaskMonitor.cpp)
component_include_directories(tasks_ui BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(tasks_ui PUBLIC lux::engine::editor::view_api lux::engine::process::process_execution PRIVATE imgui::core)
component_add_transitive_commands(tasks_ui
    "find_package(lux-engine-editor-view-api REQUIRED COMPONENTS view_api)"
    "find_package(lux-engine-process-execution REQUIRED COMPONENTS process_execution)"
    "find_package(imgui REQUIRED COMPONENTS core)")
lux_classify_target(TARGET tasks_ui LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-tasks-ui VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS tasks_ui)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_task_monitor_test test/monitor.cpp)
    target_link_libraries(editor_task_monitor_test PRIVATE tasks_ui view_host)
    lux_editor_test_options(editor_task_monitor_test)
    add_test(NAME editor.tasks.monitor COMMAND editor_task_monitor_test)
    set_tests_properties(editor.tasks.monitor PROPERTIES LABELS "editor;native;P10Q" TIMEOUT 60)
endif()
