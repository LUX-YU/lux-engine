include_component_cmake_scripts(meta)
find_package(Eigen3 CONFIG REQUIRED)
add_component(COMPONENT_NAME scene_camera NAMESPACE lux::engine::scene
    OUTPUT_NAME lux_engine_scene_camera STATIC
    SOURCE_FILES src/Camera.cpp src/CameraSchema.cpp)
component_include_directories(scene_camera
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include)
target_link_libraries(scene_camera PUBLIC lux::engine::simulation::ecs::schema
    lux::engine::core::type_info Eigen3::Eigen)
include(${PROJECT_SOURCE_DIR}/engine/domain/simulation/ecs/schema/cmake/engine_ecs_codegen.cmake)
engine_target_add_ecs_component_codegen(NAME scene_camera_component_codegen TARGET scene_camera
    SOURCE_FILE ${CMAKE_CURRENT_SOURCE_DIR}/src/CameraSchema.cpp
    HEADER ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/scene/Camera.hpp
    LOGICAL_PATH lux/engine/scene/Camera.hpp SYMBOL sceneCamera)
component_add_transitive_commands(scene_camera
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS schema)"
    "find_package(lux-engine-core REQUIRED COMPONENTS type_info)"
    "find_package(Eigen3 CONFIG REQUIRED)")
lux_classify_target(TARGET scene_camera LAYER SCENE PRODUCT COMMON ROLE LIBRARY)
lux_engine_install_components(PROJECT_NAME lux-engine-scene-camera VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine::scene COMPONENTS scene_camera)
