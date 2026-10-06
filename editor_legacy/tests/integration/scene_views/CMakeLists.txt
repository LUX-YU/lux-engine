if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_scene_fixture_legacy fixture.cpp)
    target_link_libraries(editor_scene_fixture_legacy PRIVATE scene_model_legacy editor_project_legacy
        lux::engine::scene::scene_render lux::engine::scene::scene_world_loading
        lux::engine::scene::scene_mesh_query lux::engine::scene::scene_transform
        lux::engine::function::render_features lux::engine::function::material_graph)
    lux_editor_test_options(editor_scene_fixture_legacy)
    add_test(NAME editor.scene_source_preservation COMMAND editor_scene_fixture_legacy
        "${CMAKE_CURRENT_BINARY_DIR}/preservation-$<CONFIG>" author-roundtrip)
    set_tests_properties(editor.scene_source_preservation PROPERTIES LABELS "editor;native;P12" RUN_SERIAL TRUE)
endif()
if(LUX_EDITOR_TEST_DESKTOP_GPU AND LUX_EDITOR_BUILD_TOOLCHAIN_TESTS)
    add_executable(editor_scene_views_test_legacy views.cpp)
    target_link_libraries(editor_scene_views_test_legacy PRIVATE session_execution_legacy editor_storage_legacy editor_extensions_legacy scene_ui_legacy desktop_shell_legacy material_ui_legacy flow_ui_legacy flow_module_legacy material_module_legacy scene_module_legacy scene_execution_legacy tasks_ui_legacy flowforge_persistence_legacy editor_persistence_execution_legacy editor_file_publication_legacy material_persistence_legacy
        lux::engine::scene::scene_transform lux::engine::function::ui_rendering
        lux::engine::function::render_features imgui::core)
    lux_editor_test_options(editor_scene_views_test_legacy)
    target_compile_definitions(editor_scene_views_test_legacy PRIVATE LUX_P10_R1_NATIVE=1)
    lux_editor_find_linker(P10_LINKER)
    add_test(NAME editor.scene_views_gpu COMMAND editor_scene_views_test_legacy "${P10_LINKER}" "${CMAKE_BINARY_DIR}")
    foreach(case literal signature flow-canvas material-canvas flow-queue material-queue material-lifetime lifecycle positive history)
        add_test(NAME editor.draft_source_${case} COMMAND editor_scene_views_test_legacy "${P10_LINKER}" "${CMAKE_BINARY_DIR}" "r1-${case}")
        set_tests_properties(editor.draft_source_${case} PROPERTIES LABELS "editor;P10;R1;gpu" TIMEOUT 120 RUN_SERIAL TRUE)
    endforeach()
    if(WIN32)
        target_link_libraries(editor_scene_views_test_legacy PRIVATE user32)
        add_test(NAME editor.desktop_native_input COMMAND editor_scene_views_test_legacy "${P10_LINKER}" "${CMAKE_BINARY_DIR}" native)
        set_tests_properties(editor.desktop_native_input PROPERTIES LABELS "editor;P10;desktop;gpu" TIMEOUT 120 RUN_SERIAL TRUE)
    endif()
    set_tests_properties(editor.scene_views_gpu PROPERTIES LABELS "editor;P10;gpu" TIMEOUT 120)
endif()


if(LUX_EDITOR_TEST_DESKTOP_GPU)
    add_executable(editor_ui_presentation_legacy unified_ui.cpp)
    target_link_libraries(editor_ui_presentation_legacy PRIVATE desktop_shell_legacy editor_viewport_legacy lux::engine::platform::window
        lux::engine::function::render_features lux::engine::scene::scene_transform imgui::core)
    lux_editor_test_options(editor_ui_presentation_legacy)
    foreach(mode backpressure scene-panes)
        add_test(NAME editor.presentation_${mode} COMMAND editor_ui_presentation_legacy --${mode})
        set_tests_properties(editor.presentation_${mode} PROPERTIES LABELS "editor;desktop;gpu" RUN_SERIAL TRUE TIMEOUT 120)
    endforeach()
endif()
