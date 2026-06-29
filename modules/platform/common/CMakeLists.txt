# Component: common
# -----------------
# Pure C++ foundational types shared across platform, resource, and function layers.
# Zero external dependencies — no Vulkan, no Eigen, no GLFW.

add_interface_component(
    COMPONENT_NAME common
    NAMESPACE      lux::engine::platform
)

component_include_directories(
    common
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME
        include
)
