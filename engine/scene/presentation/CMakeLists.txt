add_interface_component(
    COMPONENT_NAME scene_presentation
    NAMESPACE      lux::engine::scene
)
component_include_directories(
    scene_presentation
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)

install_components(
    PROJECT_NAME lux-engine-scene-presentation
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::scene
    COMPONENTS   scene_presentation
)
