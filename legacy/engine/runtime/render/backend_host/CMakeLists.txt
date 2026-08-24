## Process-level render backend ownership.
##
## This component owns no scene semantics. It exposes the header-only
## RenderBackendHost template that assembles the frame/control/upload channels,
## render thread and accepted-work shutdown protocol. Concrete applications
## remain responsible for windows, targets and their main loops.

add_interface_component(
    COMPONENT_NAME runtime_render_backend_host
    NAMESPACE      lux::engine::runtime
)

component_include_directories(
    runtime_render_backend_host
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME      include
)

target_link_libraries(
    runtime_render_backend_host
    INTERFACE
        lux::engine::runtime::runtime_render_scene
        lux::engine::function::render_features
        lux::engine::core::log
)

lux_classify_target(
    TARGET  runtime_render_backend_host
    LAYER   RUNTIME
    PRODUCT RUNTIME
    ROLE    COMPOSITION
)

component_add_transitive_commands(
    runtime_render_backend_host
    "find_package(lux-engine-runtime REQUIRED COMPONENTS runtime_render_scene)"
    "find_package(lux-engine-function REQUIRED COMPONENTS render_features)"
    "find_package(lux-engine-core REQUIRED COMPONENTS log)"
)
