## core::async_port — ECS-free typed asynchronous admission contract.
##
## This target deliberately owns no scheduler, queue, worker, registry or
## dependency graph. Runtime execution supplies Endpoint implementations.

add_interface_component(
    COMPONENT_NAME async_port
    NAMESPACE      lux::engine::core
)

component_include_directories(
    async_port
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME      include
)

target_link_libraries(
    async_port
    INTERFACE
        lux::cxx::compile_time
)

component_add_transitive_commands(
    async_port
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)

lux_classify_target(
    TARGET  async_port
    LAYER   CORE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)
