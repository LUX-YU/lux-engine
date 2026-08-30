find_package(stduuid CONFIG REQUIRED)

add_interface_component(
    COMPONENT_NAME identity
    NAMESPACE      lux::engine::resource
)

component_include_directories(
    identity
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)

target_link_libraries(
    identity
    INTERFACE stduuid
)

component_add_transitive_commands(
    identity
    "find_package(stduuid CONFIG REQUIRED)"
)

lux_classify_target(
    TARGET  identity
    LAYER   RESOURCE
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(
    resource_identity_contract_test
    ${CMAKE_CURRENT_SOURCE_DIR}/test/resource_identity_contract_test.cpp
)
target_link_libraries(
    resource_identity_contract_test
    PRIVATE lux::engine::resource::identity
)
if(MSVC)
    target_compile_options(resource_identity_contract_test PRIVATE /UNDEBUG)
else()
    target_compile_options(resource_identity_contract_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  resource_identity_contract_test
    LAYER   RESOURCE
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(
    NAME resource_identity_contract_test
    COMMAND resource_identity_contract_test
)
