add_executable(world_transform_precision_test world_transform_precision_test.cpp)
target_link_libraries(
    world_transform_precision_test
    PRIVATE lux::engine::ecs::transform
)
