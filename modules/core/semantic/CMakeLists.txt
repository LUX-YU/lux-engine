add_interface_component(
    COMPONENT_NAME semantic
    NAMESPACE      lux::engine::core
)

component_include_directories(
    semantic
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME      include
)

lux_classify_target(
    TARGET  semantic
    LAYER   CORE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)
