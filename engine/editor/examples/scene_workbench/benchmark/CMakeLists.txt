# Cost artifacts use ordinary allocation and real production owners. Never run them with diagnostic DLLs.
if(LUX_EDITOR_DIAGNOSTICS OR NOT WIN32)
    return()
endif()
add_executable(editor_workbench_cost LegacyCost.cpp CostSample.cpp)
target_link_libraries(editor_workbench_cost PRIVATE editor_application)
target_include_directories(editor_workbench_cost PRIVATE ${PROJECT_SOURCE_DIR}/engine/editor/scene/pinclude)
target_compile_options(editor_workbench_cost PRIVATE /UNDEBUG)
lux_classify_target(TARGET editor_workbench_cost LAYER EDITOR PRODUCT TEST ROLE COMPOSITION)

add_executable(editor_scene_cost SceneCost.cpp CostSample.cpp ../DevelopmentScene.cpp)
target_link_libraries(editor_scene_cost PRIVATE editor_application_candidate lux::engine::platform::window
    lux::engine::simulation::transform_system lux::engine::scene::scene_render_meta
    lux::engine::function::render_client_meta lux::engine::simulation::ecs::hierarchy)
target_include_directories(editor_scene_cost PRIVATE ${PROJECT_SOURCE_DIR}/engine/editor/rendering/pinclude)
target_compile_options(editor_scene_cost PRIVATE /UNDEBUG)
engine_add_type_runtime_info(NAME editor_cost_transform_meta
    TARGET_FILES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/builtin/transform/include/lux/engine/simulation/TransformSystem.hpp
    REGISTER_FUNC_NAME lux_editor_cost_transform)
engine_target_add_type_runtime_info(TARGET editor_scene_cost METAS editor_cost_transform_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
lux_classify_target(TARGET editor_scene_cost LAYER EDITOR PRODUCT TEST ROLE COMPOSITION)
