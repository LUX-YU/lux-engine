add_interface_component(
    COMPONENT_NAME engine_runtime_contracts
    NAMESPACE lux::engine
)

component_include_directories(
    engine_runtime_contracts
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    INSTALL_TIME include
)

target_link_libraries(
    engine_runtime_contracts
    INTERFACE lux::engine::core::error lux::engine::core::core_execution
)

component_add_transitive_commands(
    engine_runtime_contracts
    "find_package(lux-engine-core REQUIRED COMPONENTS error)"
    "find_package(lux-engine-core-execution REQUIRED COMPONENTS core_execution)"
)

lux_classify_target(TARGET engine_runtime_contracts LAYER ENGINE PRODUCT RUNTIME ROLE LIBRARY)

find_package(lux-cxx REQUIRED COMPONENTS container)

add_component(
    COMPONENT_NAME engine_runtime
    NAMESPACE lux::engine
    STATIC
    SOURCE_FILES
        src/Engine.cpp
        src/SceneBuilder.cpp
)

component_include_directories(
    engine_runtime
    BUILD_TIME_EXPORT ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
    INSTALL_TIME include
)

target_link_libraries(
    engine_runtime
    PUBLIC
        lux::engine::engine_context
        lux::engine::scene::scene_composition
        lux::engine::core::error
        lux::engine::core::core_execution
    PRIVATE
        lux::cxx::container
        lux::engine::core::task
        lux::engine::simulation::simulation_task
        lux::engine::engine_metadata
)

component_add_transitive_commands(
    engine_runtime
    "find_package(lux-engine-context REQUIRED COMPONENTS engine_context)"
    "find_package(lux-engine-scene REQUIRED COMPONENTS scene_composition)"
    "find_package(lux-engine-core REQUIRED COMPONENTS error)"
    "find_package(lux-engine-core REQUIRED COMPONENTS task)"
    "find_package(lux-engine-core-execution REQUIRED COMPONENTS core_execution)"
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_task)"
    "find_package(lux-cxx REQUIRED COMPONENTS container)"
    "find_package(lux-engine-runtime-metadata REQUIRED COMPONENTS engine_metadata)"
)

lux_classify_target(TARGET engine_runtime LAYER ENGINE PRODUCT RUNTIME ROLE LIBRARY)

lux_engine_install_components(
    PROJECT_NAME lux-engine-runtime
    VERSION ${PROJECT_VERSION}
    NAMESPACE lux::engine
    COMPONENTS engine_runtime_contracts engine_runtime
)

if(BUILD_TESTING)
    add_executable(engine_assembly_test test/assembly.cpp)

    target_link_libraries(
        engine_assembly_test
        PRIVATE
            lux::engine::engine_initialization
            lux::engine::engine_metadata
    )

    if(MSVC)
        target_compile_options(engine_assembly_test PRIVATE /UNDEBUG)
    else()
        target_compile_options(engine_assembly_test PRIVATE -UNDEBUG)
    endif()

    add_test(NAME engine.assembly COMMAND engine_assembly_test)
    set_tests_properties(engine.assembly PROPERTIES TIMEOUT 30)
endif()
