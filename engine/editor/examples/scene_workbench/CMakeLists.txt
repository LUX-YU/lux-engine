add_executable(lux_editor_er1 main.cpp DevelopmentScene.cpp)
target_link_libraries(lux_editor_er1 PRIVATE editor_application_candidate
    lux::engine::simulation::transform_system lux::engine::scene::scene_render_meta
    lux::engine::function::render_client_meta lux::engine::simulation::ecs::hierarchy)
include_component_cmake_scripts(meta)
engine_add_type_runtime_info(NAME editor_demo_transform_meta
    TARGET_FILES ${PROJECT_SOURCE_DIR}/engine/domain/simulation/builtin/transform/include/lux/engine/simulation/TransformSystem.hpp
    REGISTER_FUNC_NAME lux_editor_demo_transform)
engine_target_add_type_runtime_info(TARGET lux_editor_er1 METAS editor_demo_transform_meta
    OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/meta_gen)
