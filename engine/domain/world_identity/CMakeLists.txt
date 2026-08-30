find_package(stduuid CONFIG REQUIRED)

add_interface_component(
    COMPONENT_NAME domain_world_identity
    NAMESPACE      lux::engine::domain
)
component_include_directories(
    domain_world_identity
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME      include
)
target_link_libraries(domain_world_identity INTERFACE stduuid)
component_add_transitive_commands(
    domain_world_identity
    "find_package(stduuid CONFIG REQUIRED)"
)
lux_classify_target(
    TARGET  domain_world_identity
    LAYER   DOMAIN
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(
    domain_world_identity_test
    test/world_object_id_test.cpp
)
target_link_libraries(
    domain_world_identity_test
    PRIVATE lux::engine::domain::domain_world_identity
)
if(MSVC)
    target_compile_options(domain_world_identity_test PRIVATE /UNDEBUG)
else()
    target_compile_options(domain_world_identity_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  domain_world_identity_test
    LAYER   DOMAIN
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME domain_world_identity_test COMMAND domain_world_identity_test)

install_components(
    PROJECT_NAME lux-engine-domain-world-identity
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::domain
    COMPONENTS   domain_world_identity
)
