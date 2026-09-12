add_executable(editor_application_lifecycle_test ../ApplicationLifecycleTest.cpp ../../DevelopmentScene.cpp)
target_link_libraries(editor_application_lifecycle_test PRIVATE editor_application
    lux::engine::simulation::transform_system lux::engine::scene::scene_render_meta
    lux::engine::function::render_client_meta lux::engine::simulation::ecs::hierarchy)
target_compile_options(editor_application_lifecycle_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
if(LUX_EDITOR_DIAGNOSTICS)
    target_compile_definitions(editor_application_lifecycle_test PRIVATE LUX_EDITOR_DIAGNOSTICS)
    target_include_directories(editor_application_lifecycle_test PRIVATE
        ${PROJECT_SOURCE_DIR}/engine/editor/rendering/pinclude)
endif()
engine_add_type_runtime_info(NAME editor_lifecycle_transform_meta
    TARGET_FILES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/builtin/transform/include/lux/engine/simulation/TransformSystem.hpp
    REGISTER_FUNC_NAME lux_editor_demo_transform)
engine_target_add_type_runtime_info(TARGET editor_application_lifecycle_test METAS editor_lifecycle_transform_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
lux_classify_target(TARGET editor_application_lifecycle_test LAYER EDITOR PRODUCT TEST ROLE COMPOSITION)
# Requires an actual window/Vulkan device; run explicitly in the serial GPU qualification.
add_test(NAME editor.development_scene.metadata COMMAND editor_application_lifecycle_test metadata)
