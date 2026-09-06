add_interface_component(
    COMPONENT_NAME scene_presentation
    NAMESPACE      lux::engine::scene
)
component_include_directories(
    scene_presentation
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)
lux_classify_target(
    TARGET  scene_presentation
    LAYER   SCENE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

install_components(
    PROJECT_NAME lux-engine-scene-presentation
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_presentation
)
