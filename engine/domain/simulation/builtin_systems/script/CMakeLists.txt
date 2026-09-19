generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_SCRIPT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/script_system/visibility.h
)

add_component(
    COMPONENT_NAME simulation_script
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_script
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptBindingAuthoring.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptBindings.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptInstances.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptPreparer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptExecution.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptEventWaits.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptTimers.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptCompletionIngress.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptSystem.cpp
)
component_include_directories(
    simulation_script
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_include_directories(
    simulation_script
    PUBLIC $<INSTALL_INTERFACE:include/lux/engine/simulation/abilities>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/pinclude
)
target_compile_definitions(
    simulation_script
    PRIVATE
        LUX_ENGINE_SIMULATION_SCRIPT_LIBRARY
        $<$<CONFIG:Debug>:LUX_SCRIPT_OWNER_AFFINITY_PROBE=1>
)
target_link_libraries(
    simulation_script
    PUBLIC
        lux::engine::simulation::description
        lux::engine::simulation::system
        lux::engine::simulation::simulation_scripting_core
        lux::engine::simulation::ecs::core
        lux::engine::function::script_core
        lux::engine::resource::identity
        lux::engine::function::script_artifact
    PRIVATE
        lux::cxx::container
)

option(LUX_SCRIPT_HOTPATH_OBSERVATION "Count Script Hook candidate and visited records (diagnostics only)" OFF)
if(LUX_SCRIPT_HOTPATH_OBSERVATION)
    target_compile_definitions(simulation_script PRIVATE LUX_SCRIPT_HOTPATH_OBSERVATION=1)
endif()

find_package(lux-cxx REQUIRED COMPONENTS container)

component_add_transitive_commands(
    simulation_script
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_description)"
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_scripting_core)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS identity)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_artifact)"
)

include_component_cmake_scripts(script_core)
lux_script_abilities(
    TARGET simulation_script
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/simulation/abilities/DelayAbility.hpp
    LOGICAL_PATHS lux/engine/simulation/abilities/DelayAbility.hpp
)
get_target_property(_delay_ability_generated_dir simulation_script LUX_SCRIPT_ABILITY_GENERATED_DIR)
install(
    FILES ${_delay_ability_generated_dir}/DelayAbility.ability.generated.hpp
    DESTINATION include/lux/engine/simulation/abilities
    COMPONENT lux_sdk
)
