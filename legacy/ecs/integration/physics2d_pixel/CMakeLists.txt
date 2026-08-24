add_component(
    COMPONENT_NAME  physics2d_pixel_integration
    NAMESPACE       lux::engine::ecs
    STATIC
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/PixelCollisionProbe.cpp
)

lux_classify_target(
    TARGET  physics2d_pixel_integration
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

component_include_directories(
    physics2d_pixel_integration
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME
        include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
target_link_libraries(
    physics2d_pixel_integration
    PUBLIC
        lux::engine::ecs::core
        lux::engine::ecs::physics
        lux::engine::ecs::pixel
)

component_add_transitive_commands(
    physics2d_pixel_integration
    "find_package(lux-engine-ecs REQUIRED COMPONENTS core physics pixel)"
)
