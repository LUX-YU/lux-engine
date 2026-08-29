generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_CORE_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_CORE_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_CORE_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/core/visibility.h
)

add_component(
    COMPONENT_NAME simulation_core
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_core
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Simulation.cpp
)

component_include_directories(
    simulation_core
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(simulation_core PRIVATE LUX_ENGINE_SIMULATION_CORE_LIBRARY)
target_link_libraries(
    simulation_core
    PUBLIC
        lux::engine::simulation::description
        lux::engine::simulation::system
        lux::engine::simulation::ecs::core
        lux::engine::simulation::ecs::task
        lux::engine::core::task
)
component_add_transitive_commands(
    simulation_core
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_description)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core ecs_task)"
    "find_package(lux-engine-core REQUIRED COMPONENTS task)"
)

lux_classify_target(
    TARGET  simulation_core
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    COMPOSITION
)

add_executable(simulation_runtime_test test/simulation_runtime_test.cpp)
target_link_libraries(
    simulation_runtime_test
    PRIVATE lux::engine::simulation::simulation_core
)
if(MSVC)
    target_compile_options(simulation_runtime_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_runtime_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_runtime_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    COMPOSITION
)
add_test(NAME simulation_runtime_test COMMAND simulation_runtime_test)

install_components(
    PROJECT_NAME lux-engine-simulation-core
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::simulation
    COMPONENTS   simulation_core
)
