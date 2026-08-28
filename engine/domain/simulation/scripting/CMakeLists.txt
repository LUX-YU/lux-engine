add_component(
    STATIC
    COMPONENT_NAME simulation_scripting
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_scripting
    SOURCE_FILES src/ScriptScripting.cpp
)

component_include_directories(
    simulation_scripting
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../systems/script/include
    INSTALL_TIME include
)

target_link_libraries(
    simulation_scripting
    PUBLIC
        lux::engine::simulation::description
        lux::engine::simulation::system
        lux::engine::simulation::ecs::core
        lux::engine::function::script_core
        lux::engine::resource::asset
        lux::engine::resource::script_asset
        lux::engine::world
)

component_add_transitive_commands(
    simulation_scripting
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_description)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-resource-script REQUIRED COMPONENTS script_asset)"
    "find_package(lux-engine-world REQUIRED COMPONENTS world)"
)

lux_classify_target(
    TARGET  simulation_scripting
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    FOUNDATION
)
