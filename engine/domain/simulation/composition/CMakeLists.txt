generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_COMPOSITION_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_COMPOSITION_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_COMPOSITION_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/composition/visibility.h
)

add_component(
    COMPONENT_NAME simulation_composition
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_composition
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/Simulation.cpp
)
add_library(lux::engine::simulation::composition ALIAS simulation_composition)

component_include_directories(
    simulation_composition
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(simulation_composition PRIVATE LUX_ENGINE_SIMULATION_COMPOSITION_LIBRARY)
target_link_libraries(
    simulation_composition
    PUBLIC
        lux::engine::simulation::description
        lux::engine::simulation::system
        lux::engine::simulation::ecs::core
        lux::engine::simulation::ecs::task
        lux::engine::core::task
        lux::engine::simulation::simulation_scripting_core
)
component_add_transitive_commands(
    simulation_composition
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_description)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core ecs_task)"
    "find_package(lux-engine-core REQUIRED COMPONENTS task)"
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_scripting_core)"
    "add_library(lux::engine::simulation::composition INTERFACE IMPORTED GLOBAL)"
    "set_property(TARGET lux::engine::simulation::composition PROPERTY INTERFACE_LINK_LIBRARIES lux::engine::simulation::simulation_composition)"
)

install_components(
    PROJECT_NAME lux-engine-simulation-composition
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::simulation
    COMPONENTS   simulation_composition
)
