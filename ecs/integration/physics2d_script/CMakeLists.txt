add_component(
    COMPONENT_NAME  physics2d_script_integration
    NAMESPACE       lux::engine::ecs
    STATIC
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Physics2DScriptIntegration.cpp
)

lux_classify_target(
    TARGET  physics2d_script_integration
    LAYER   ECS
    PRODUCT RUNTIME
    ROLE    INTEGRATION
)

component_include_directories(
    physics2d_script_integration
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME
        include
)
target_link_libraries(
    physics2d_script_integration
    PUBLIC
        lux::engine::ecs::physics2d
        lux::engine::ecs::scripting
        lux::engine::core::meta
)

component_add_transitive_commands(
    physics2d_script_integration
    "find_package(lux-engine-ecs REQUIRED COMPONENTS physics2d scripting)"
    "find_package(lux-engine-core REQUIRED COMPONENTS meta)"
)
