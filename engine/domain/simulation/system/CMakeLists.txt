find_package(lux-cxx REQUIRED COMPONENTS compile_time container)
find_package(EnTT CONFIG REQUIRED)

generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_SYSTEM_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_SYSTEM_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_SYSTEM_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/system/visibility.h
)

add_component(
    COMPONENT_NAME system
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_system
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/SimulationSystemRegistry.cpp
)

component_include_directories(
    system
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)

target_compile_definitions(system PRIVATE LUX_ENGINE_SIMULATION_SYSTEM_LIBRARY)
target_link_libraries(
    system
    PUBLIC
        lux::engine::simulation::description
        lux::engine::core::serialization
        lux::engine::simulation::ecs::core
        EnTT::EnTT
        lux::cxx::compile_time
        lux::cxx::container
)
component_add_transitive_commands(
    system
    "find_package(lux-cxx REQUIRED COMPONENTS compile_time container)"
    "find_package(EnTT CONFIG REQUIRED)"
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_description)"
    "find_package(lux-engine-core REQUIRED COMPONENTS serialization)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core)"
)

install_components(
    PROJECT_NAME lux-engine-simulation-system
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::simulation
    COMPONENTS   system
)
