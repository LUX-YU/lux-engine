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
)
component_add_transitive_commands(
    simulation_composition
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_description)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core ecs_task)"
    "find_package(lux-engine-core REQUIRED COMPONENTS task)"
    "add_library(lux::engine::simulation::composition INTERFACE IMPORTED GLOBAL)"
    "set_property(TARGET lux::engine::simulation::composition PROPERTY INTERFACE_LINK_LIBRARIES lux::engine::simulation::simulation_composition)"
)

lux_classify_target(
    TARGET  simulation_composition
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    COMPOSITION
)

add_executable(simulation_composition_test test/simulation_composition_test.cpp)
target_link_libraries(
    simulation_composition_test
    PRIVATE lux::engine::simulation::composition
)
if(MSVC)
    target_compile_options(simulation_composition_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_composition_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_composition_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    COMPOSITION
)
add_test(NAME simulation_composition_test COMMAND simulation_composition_test)

install_components(
    PROJECT_NAME lux-engine-simulation-composition
    VERSION      ${PROJECT_VERSION}
    NAMESPACE    lux::engine::simulation
    COMPONENTS   simulation_composition
)
