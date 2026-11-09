find_package(lux-cxx REQUIRED COMPONENTS container)

add_component(
    COMPONENT_NAME engine_metadata
    NAMESPACE lux::engine
    STATIC
    SOURCE_FILES src/EngineMetaFunctionRegistry.cpp
)

component_include_directories(
    engine_metadata
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)

target_link_libraries(
    engine_metadata
    PUBLIC
        lux::engine::scene::scene_composition
        lux::engine::simulation::system
        lux::engine::ecs::ecs_schema
        lux::engine::world::partition
        lux::engine::scene::scene_description
        STDEXEC::stdexec
    PRIVATE lux::cxx::container
)

component_add_transitive_commands(
    engine_metadata
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-ecs REQUIRED COMPONENTS ecs_schema)"
    "find_package(lux-engine-world-partition REQUIRED COMPONENTS world_partition)"
    "find_package(lux-engine-scene-description REQUIRED COMPONENTS scene_description)"
    "find_package(stdexec CONFIG REQUIRED GLOBAL)"
    "find_package(lux-cxx REQUIRED COMPONENTS container)"
)

lux_classify_target(TARGET engine_metadata LAYER ENGINE PRODUCT RUNTIME ROLE LIBRARY)

lux_engine_install_components(
    PROJECT_NAME lux-engine-runtime-metadata
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine
    COMPONENTS engine_metadata
)
