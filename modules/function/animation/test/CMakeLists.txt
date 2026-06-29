# lux::engine::function::animation — unit tests.
#
# Enable with -DENABLE_ANIMATION_TEST=ON. Each test is a standalone
# self-checking executable returning exit code 0 on success, !=0 on failure
# (mirrors flowforge_cfg_test convention — no GoogleTest dep).

add_executable(pose_sampling_test    pose_sampling_test.cpp)
target_link_libraries(pose_sampling_test    PRIVATE animation)

add_executable(skinning_matrix_test  skinning_matrix_test.cpp)
target_link_libraries(skinning_matrix_test  PRIVATE animation)
