# Reuse the production cooker to regenerate the GPU fixture from this checkout.
add_subdirectory(${PROJECT_SOURCE_DIR}/engine/toolchain/scene_seed ${CMAKE_CURRENT_BINARY_DIR}/seed)

add_executable(editor_scene_gpu_test SceneGpuTest.cpp ../DevelopmentScene.cpp)
target_link_libraries(editor_scene_gpu_test PRIVATE editor_application
    lux::engine::platform::window
    lux::engine::simulation::transform_system lux::engine::scene::scene_render_meta
    lux::engine::function::render_client_meta lux::engine::simulation::ecs::hierarchy)
target_include_directories(editor_scene_gpu_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/editor/rendering/pinclude)
target_include_directories(editor_scene_gpu_test PRIVATE ${PROJECT_SOURCE_DIR}/engine/editor/sessions/scene/pinclude)
target_compile_options(editor_scene_gpu_test PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>)
engine_add_type_runtime_info(NAME editor_test_transform_meta
    TARGET_FILES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/builtin/transform/include/lux/engine/simulation/TransformSystem.hpp
    REGISTER_FUNC_NAME lux_editor_demo_transform)
engine_target_add_type_runtime_info(TARGET editor_scene_gpu_test METAS editor_test_transform_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
lux_classify_target(TARGET editor_scene_gpu_test LAYER EDITOR PRODUCT TEST ROLE COMPOSITION)
add_subdirectory(lifecycle)
