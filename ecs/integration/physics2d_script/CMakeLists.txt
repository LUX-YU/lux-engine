add_library(
    physics2d_script_integration STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/Physics2DScriptIntegration.cpp
)
add_library(
    lux::engine::ecs::physics2d_script_integration
    ALIAS physics2d_script_integration
)

lux_classify_target(
    TARGET  physics2d_script_integration
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

target_include_directories(
    physics2d_script_integration
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(
    physics2d_script_integration
    PUBLIC
        lux::engine::ecs::physics2d
        lux::engine::ecs::scripting
        lux::engine::core::meta
)
