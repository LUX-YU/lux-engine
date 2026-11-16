add_interface_component(
    COMPONENT_NAME engine_context
    NAMESPACE lux::engine
)

component_include_directories(
    engine_context
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)

lux_classify_target(TARGET engine_context LAYER SERVICES PRODUCT RUNTIME ROLE LIBRARY)

target_link_libraries(engine_context INTERFACE lux::engine::core::error)

component_add_transitive_commands(engine_context "find_package(lux-engine-core REQUIRED COMPONENTS error)")

lux_engine_install_components(
    PROJECT_NAME lux-engine-context
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine
    COMPONENTS engine_context
)
