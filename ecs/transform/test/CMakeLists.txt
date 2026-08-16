add_executable(world_transform_precision_test world_transform_precision_test.cpp)
target_link_libraries(
    world_transform_precision_test
    PRIVATE lux::engine::ecs::transform
)

add_test(
    NAME world_transform_precision_test
    COMMAND world_transform_precision_test
)
set_tests_properties(
    world_transform_precision_test
    PROPERTIES LABELS "cpu;world" TIMEOUT 30
)
