find_package(lux-cxx REQUIRED COMPONENTS core)

add_interface_component(
    COMPONENT_NAME scene_api
    NAMESPACE      lux::engine::scene
)

component_include_directories(
    scene_api
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME
        include
)

target_link_libraries(
    scene_api
    INTERFACE
        lux::cxx::core
)

component_add_transitive_commands(
    scene_api
    "find_package(lux-cxx REQUIRED COMPONENTS core)"
)

lux_classify_target(
    TARGET  scene_api
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(
    scene_feature_id_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/scene_feature_id_test.cpp
)
target_link_libraries(
    scene_feature_id_test
    PRIVATE lux::engine::scene::scene_api
)
lux_classify_target(
    TARGET  scene_feature_id_test
    LAYER   RUNTIME
    PRODUCT TEST
    ROLE    DOMAIN
)
