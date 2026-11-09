add_component(
    COMPONENT_NAME engine_initialization
    NAMESPACE lux::engine
    STATIC
    SOURCE_FILES src/EngineContext.cpp src/Init.cpp
)

component_include_directories(
    engine_initialization
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)

target_link_libraries(
    engine_initialization
    PUBLIC
        lux::engine::engine_runtime
        lux::engine::engine_context
        lux::engine::assets::engine_assets
    PRIVATE lux::engine::core::object lux::engine::core::log
)

component_add_transitive_commands(
    engine_initialization
    "find_package(lux-engine-runtime REQUIRED COMPONENTS engine_runtime)"
    "find_package(lux-engine-context REQUIRED COMPONENTS engine_context)"
    "find_package(lux-engine-assets REQUIRED COMPONENTS engine_assets)"
    "find_package(lux-engine-core REQUIRED COMPONENTS object log)"
)

if(LUX_ENGINE_WITH_RENDER)
    target_link_libraries(engine_initialization PUBLIC lux::engine::systems::system_render)
    component_add_transitive_commands(
        engine_initialization
        "find_package(lux-engine-systems-render REQUIRED COMPONENTS system_render)"
    )
endif()

lux_engine_install_components(
    PROJECT_NAME lux-engine-initialization
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine
    COMPONENTS engine_initialization
)

lux_classify_target(TARGET engine_initialization LAYER ENGINE PRODUCT RUNTIME ROLE LIBRARY)
