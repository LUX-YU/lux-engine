find_package(lux-cxx REQUIRED COMPONENTS compile_time)

add_interface_component(
    COMPONENT_NAME partition_core
    NAMESPACE      lux::engine::partition
)
component_include_directories(
    partition_core
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)
target_link_libraries(partition_core INTERFACE lux::cxx::compile_time)
component_add_transitive_commands(
    partition_core
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time)"
)
lux_classify_target(
    TARGET  partition_core
    LAYER   DOMAIN
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(
    partition_core_test
    test/partition_core_test.cpp
)
target_link_libraries(
    partition_core_test
    PRIVATE lux::engine::partition::partition_core
)
if(MSVC)
    target_compile_options(partition_core_test PRIVATE /UNDEBUG)
else()
    target_compile_options(partition_core_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  partition_core_test
    LAYER   DOMAIN
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME partition_core_test COMMAND partition_core_test)

install_components(
    PROJECT_NAME lux-engine-domain-partition
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::partition
    COMPONENTS   partition_core
)
