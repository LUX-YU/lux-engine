if(LUX_EDITOR_BUILD_NATIVE_TESTS)
    add_executable(editor_projection_test_legacy projection.cpp)
    target_link_libraries(editor_projection_test_legacy PRIVATE scene_projection_legacy editor_viewport_legacy lux::engine::scene::scene_transform)
    lux_editor_test_options(editor_projection_test_legacy)
    add_test(NAME editor.projection.cost COMMAND editor_projection_test_legacy cost)
    set_tests_properties(editor.projection.cost PROPERTIES LABELS "editor;native;EC1")
    add_test(NAME editor.projection.reset_capacity COMMAND editor_projection_test_legacy)
    set_tests_properties(editor.projection.reset_capacity PROPERTIES LABELS "editor;native;P07")
endif()



# This executable inspects backend-owned state and therefore lives in the render feature test directory.
if(LUX_EDITOR_BUILD_NATIVE_TESTS AND LUX_BUILD_PACKED_RENDER_CONTENT)
    add_executable(render_view_binding_p07_test_legacy ${PROJECT_SOURCE_DIR}/modules/function/render/features/test/view_binding.cpp)
    target_link_libraries(render_view_binding_p07_test_legacy PRIVATE lux::engine::function::render_features)
    target_include_directories(render_view_binding_p07_test_legacy PRIVATE
        ${PROJECT_SOURCE_DIR}/modules/function/render/features/sinclude
        ${PROJECT_SOURCE_DIR}/modules/function/render/vulkan/sinclude)
    lux_editor_test_options(render_view_binding_p07_test_legacy)
    add_test(NAME render.features.view_binding COMMAND render_view_binding_p07_test_legacy)
    set_tests_properties(render.features.view_binding PROPERTIES LABELS "native;P07;render")
endif()
