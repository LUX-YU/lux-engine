add_component(COMPONENT_NAME scene_projection NAMESPACE lux::engine::editor STATIC
    SOURCE_FILES src/ResourceStatus.cpp src/ViewportPresentation.cpp src/SceneProjection.cpp)
component_include_directories(scene_projection BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include INSTALL_TIME include)
target_link_libraries(scene_projection PUBLIC lux::engine::editor::scene_model
    lux::engine::scene::scene_composition lux::engine::scene::scene_render
    lux::engine::scene::scene_world_loading lux::engine::process::world_loading)
component_add_transitive_commands(scene_projection
    "find_package(lux-engine-editor-scene-model REQUIRED COMPONENTS scene_model)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-scene-render REQUIRED COMPONENTS scene_render)"
    "find_package(lux-engine-scene-world-loading REQUIRED COMPONENTS scene_world_loading)"
    "find_package(lux-engine-process-world-loading REQUIRED COMPONENTS process_world_loading)")
lux_classify_target(TARGET scene_projection LAYER EDITOR PRODUCT EDITOR ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-editor-scene-projection VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::editor COMPONENTS scene_projection)

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_projection_test test/projection.cpp)
    target_link_libraries(editor_projection_test PRIVATE scene_projection lux::engine::scene::scene_transform)
    lux_editor_test_options(editor_projection_test)
    add_test(NAME editor.projection.reset_capacity COMMAND editor_projection_test)
    set_tests_properties(editor.projection.reset_capacity PROPERTIES LABELS "editor;native;P07")
endif()

if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_highlight_p07_test test/highlight.cpp)
    target_link_libraries(editor_highlight_p07_test PRIVATE scene_projection)
    lux_editor_test_options(editor_highlight_p07_test)
    add_test(NAME editor.projection.highlight_backend COMMAND editor_highlight_p07_test)
    set_tests_properties(editor.projection.highlight_backend PROPERTIES LABELS "editor;native;P07")
endif()

# This executable inspects backend-owned state and therefore lives in the render feature test directory.
if(LUX_EDITOR_BUILD_NATIVE_TESTS AND LUX_BUILD_PACKED_RENDER_CONTENT)
    add_executable(render_view_binding_p07_test ${PROJECT_SOURCE_DIR}/modules/function/render/features/test/view_binding.cpp)
    target_link_libraries(render_view_binding_p07_test PRIVATE lux::engine::function::render_features)
    lux_editor_test_options(render_view_binding_p07_test)
    add_test(NAME render.features.view_binding COMMAND render_view_binding_p07_test)
    set_tests_properties(render.features.view_binding PROPERTIES LABELS "native;P07;render")
endif()
