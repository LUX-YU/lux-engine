find_package(lux-cxx REQUIRED COMPONENTS compile_time)

add_interface_component(
    COMPONENT_NAME partition_identity
    NAMESPACE      lux::engine::partition
)
add_library(lux::engine::partition::identity ALIAS partition_identity)
component_include_directories(
    partition_identity
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)
target_link_libraries(partition_identity INTERFACE lux::cxx::compile_time)
component_add_transitive_commands(
    partition_identity
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
    "add_library(lux::engine::partition::identity INTERFACE IMPORTED GLOBAL)"
    "set_property(TARGET lux::engine::partition::identity PROPERTY INTERFACE_LINK_LIBRARIES lux::engine::partition::partition_identity)"
)
lux_classify_target(
    TARGET  partition_identity
    LAYER   DOMAIN
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

install_components(
    PROJECT_NAME lux-engine-partition-identity
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::partition
    COMPONENTS   partition_identity
)
