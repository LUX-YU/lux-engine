add_component(
    STATIC
    COMPONENT_NAME simulation_scripting_core
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_scripting_core
    SOURCE_FILES src/ScriptScripting.cpp
)

component_include_directories(
    simulation_scripting_core
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../systems/script/include
    INSTALL_TIME include
)

target_link_libraries(
    simulation_scripting_core
    PUBLIC
        lux::engine::simulation::description
        lux::engine::simulation::system
        lux::engine::simulation::ecs::core
        lux::engine::function::script_core
        lux::engine::resource::asset
        lux::engine::function::script_artifact
        lux::engine::world
)

component_add_transitive_commands(
    simulation_scripting_core
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_description)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_artifact)"
    "find_package(lux-engine-world REQUIRED COMPONENTS world)"
)

lux_classify_target(
    TARGET  simulation_scripting_core
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)
