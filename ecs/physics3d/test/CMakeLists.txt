add_executable(physics3d_scene_test physics3d_scene_test.cpp)
target_link_libraries(
    physics3d_scene_test
    PRIVATE lux::engine::ecs::physics3d
)
if(MSVC)
    target_compile_options(physics3d_scene_test PRIVATE /UNDEBUG)
else()
    target_compile_options(physics3d_scene_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  physics3d_scene_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    DOMAIN
)
