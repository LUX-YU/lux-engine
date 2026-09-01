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

add_executable(
    partition_identity_test
    test/partition_identity_test.cpp
)
target_link_libraries(
    partition_identity_test
    PRIVATE lux::engine::partition::identity
)
if(MSVC)
    target_compile_options(partition_identity_test PRIVATE /UNDEBUG)
else()
    target_compile_options(partition_identity_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  partition_identity_test
    LAYER   DOMAIN
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME partition_identity_test COMMAND partition_identity_test)

install_components(
    PROJECT_NAME lux-engine-partition-identity
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::partition
    COMPONENTS   partition_identity
)
