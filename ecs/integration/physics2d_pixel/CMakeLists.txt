add_library(
    physics2d_pixel_integration STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/PixelCollisionProbe.cpp
)
add_library(
    lux::engine::ecs::physics2d_pixel_integration
    ALIAS physics2d_pixel_integration
)

lux_classify_target(
    TARGET  physics2d_pixel_integration
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

target_include_directories(
    physics2d_pixel_integration
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
target_link_libraries(
    physics2d_pixel_integration
    PUBLIC
        lux::engine::ecs::core
        lux::engine::ecs::physics
        lux::engine::ecs::pixel
)
