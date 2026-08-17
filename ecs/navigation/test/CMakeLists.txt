add_executable(
    navigation3d_system_test
    navigation3d_system_test.cpp
)
target_link_libraries(
    navigation3d_system_test
    PRIVATE lux::engine::ecs::navigation3d
)
if(MSVC)
    target_compile_options(navigation3d_system_test PRIVATE /UNDEBUG)
else()
    target_compile_options(navigation3d_system_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  navigation3d_system_test
    LAYER   ECS
    PRODUCT TEST
    ROLE    DOMAIN
)
