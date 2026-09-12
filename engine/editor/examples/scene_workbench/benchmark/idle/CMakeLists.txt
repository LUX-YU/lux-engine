# Separate finite owner traversal sample. It has zero Views and makes no rendering throughput claim.
add_executable(editor_scene_idle_cost IdleSceneCost.cpp ../CostSample.cpp ../../DevelopmentScene.cpp)
target_link_libraries(editor_scene_idle_cost PRIVATE editor_application lux::engine::platform::window
    lux::engine::simulation::transform_system lux::engine::scene::scene_render_meta
    lux::engine::function::render_client_meta lux::engine::simulation::ecs::hierarchy)
target_include_directories(editor_scene_idle_cost PRIVATE
    ${PROJECT_SOURCE_DIR}/engine/editor/rendering/pinclude
    ${PROJECT_SOURCE_DIR}/engine/editor/sessions/scene/pinclude)
target_compile_options(editor_scene_idle_cost PRIVATE /UNDEBUG)
engine_add_type_runtime_info(NAME editor_idle_transform_meta
    TARGET_FILES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/builtin/transform/include/lux/engine/simulation/TransformSystem.hpp
    REGISTER_FUNC_NAME lux_editor_idle_transform)
engine_target_add_type_runtime_info(TARGET editor_scene_idle_cost METAS editor_idle_transform_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/idle_meta_gen)
lux_classify_target(TARGET editor_scene_idle_cost LAYER EDITOR PRODUCT TEST ROLE COMPOSITION)
