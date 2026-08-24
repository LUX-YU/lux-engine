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

add_executable(
    static_collider_batch3d_codec_test
    static_collider_batch3d_codec_test.cpp)
target_link_libraries(
    static_collider_batch3d_codec_test
    PRIVATE lux::cxx::algorithm lux::engine::ecs::physics3d)
if(MSVC)
    target_compile_options(
        static_collider_batch3d_codec_test PRIVATE /UNDEBUG)
else()
    target_compile_options(
        static_collider_batch3d_codec_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET static_collider_batch3d_codec_test
    LAYER ECS PRODUCT TEST ROLE DOMAIN)
