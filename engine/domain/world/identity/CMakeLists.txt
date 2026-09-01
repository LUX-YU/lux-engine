find_package(stduuid CONFIG REQUIRED)

add_interface_component(
    COMPONENT_NAME world_identity
    NAMESPACE      lux::engine::world
)
add_library(lux::engine::world::identity ALIAS world_identity)
component_include_directories(
    world_identity
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME      include
)
target_link_libraries(world_identity INTERFACE stduuid)
component_add_transitive_commands(
    world_identity
    "find_package(stduuid CONFIG REQUIRED)"
    "add_library(lux::engine::world::identity INTERFACE IMPORTED GLOBAL)"
    "set_property(TARGET lux::engine::world::identity PROPERTY INTERFACE_LINK_LIBRARIES lux::engine::world::world_identity)"
)
lux_classify_target(
    TARGET  world_identity
    LAYER   DOMAIN
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)

add_executable(
    world_identity_test
    test/world_object_id_test.cpp
)
target_link_libraries(
    world_identity_test
    PRIVATE lux::engine::world::identity
)
if(MSVC)
    target_compile_options(world_identity_test PRIVATE /UNDEBUG)
else()
    target_compile_options(world_identity_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  world_identity_test
    LAYER   DOMAIN
    PRODUCT TEST
    ROLE    FOUNDATION
)
add_test(NAME world_identity_test COMMAND world_identity_test)

install_components(
    PROJECT_NAME lux-engine-world-identity
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::world
    COMPONENTS   world_identity
)
