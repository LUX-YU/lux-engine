include_component_cmake_scripts(meta)

find_package(Eigen3 CONFIG REQUIRED)

add_component(
    COMPONENT_NAME scene_view
    NAMESPACE lux::engine::scene
    OUTPUT_NAME lux_engine_scene_view
    STATIC
    SOURCE_FILES src/ViewComponent.cpp src/ViewSchema.cpp
)

component_include_directories(
    scene_view
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_link_libraries(
    scene_view
    PUBLIC lux::engine::ecs::ecs_schema lux::engine::core::type_info Eigen3::Eigen
)

include(${PROJECT_SOURCE_DIR}/engine/foundation/ecs/schema/cmake/engine_ecs_codegen.cmake)

engine_target_add_ecs_component_codegen(
    NAME scene_view_component_codegen
    TARGET scene_view
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/ViewSchema.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/scene/ViewComponent.hpp
    LOGICAL_PATH lux/engine/scene/ViewComponent.hpp
    SYMBOL sceneView
)

component_add_transitive_commands(
    scene_view
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_schema)"
    "find_package(lux-engine-core REQUIRED COMPONENTS type_info)"
    "find_package(Eigen3 CONFIG REQUIRED)"
)

lux_classify_target(TARGET scene_view LAYER DOMAIN PRODUCT COMMON ROLE LIBRARY)

lux_engine_install_components(
    PROJECT_NAME lux-engine-scene-view
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::scene
    COMPONENTS scene_view
)
