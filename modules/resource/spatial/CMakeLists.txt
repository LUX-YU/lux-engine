add_interface_component(
    COMPONENT_NAME spatial
    NAMESPACE      lux::engine::resource
)

lux_classify_target(
    TARGET  spatial
    LAYER   RESOURCE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

component_include_directories(
    spatial
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME
        include
)

target_link_libraries(
    spatial
    INTERFACE lux::engine::core::meta
)
component_add_transitive_commands(
    spatial
    "find_package(lux-engine-core REQUIRED COMPONENTS meta)"
)

add_executable(
    spatial_contract_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/spatial_contract_test.cpp
)
target_link_libraries(
    spatial_contract_test
    PRIVATE lux::engine::resource::spatial
)
if(MSVC)
    target_compile_options(spatial_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(spatial_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  spatial_contract_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME spatial_contract_test COMMAND spatial_contract_test)
set_tests_properties(
    spatial_contract_test
    PROPERTIES LABELS "cpu;resource;spatial" TIMEOUT 30
)
