generate_visibility_header(
    ENABLE_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_LIBRARY
    PUBLIC_MACRO_NAME       LUX_ENGINE_SIMULATION_SCRIPT_PUBLIC
    DISABLE_DLL_MACRO_NAME  LUX_ENGINE_SIMULATION_SCRIPT_DLL_DISABLE
    GENERATE_FILE_PATH      lux/engine/simulation/systems/script/visibility.h
)

add_component(
    COMPONENT_NAME simulation_script
    NAMESPACE      lux::engine::simulation
    OUTPUT_NAME    lux_engine_simulation_script
    SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptSystemDescription.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptSystemDescriptionCodec.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ScriptSystem.cpp
)
component_include_directories(
    simulation_script
    BUILD_TIME_EXPORT
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LUX_GENERATE_HEADER_DIR}
    INSTALL_TIME include
)
target_compile_definitions(
    simulation_script
    PRIVATE LUX_ENGINE_SIMULATION_SCRIPT_LIBRARY
)
target_link_libraries(
    simulation_script
    PUBLIC
        lux::engine::simulation::description
        lux::engine::simulation::system
        lux::engine::simulation::simulation_scripting
        lux::engine::simulation::ecs::core
        lux::engine::function::script_core
        lux::engine::resource::asset
        lux::engine::resource::script_asset
        lux::engine::world
)
component_add_transitive_commands(
    simulation_script
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_description)"
    "find_package(lux-engine-simulation REQUIRED COMPONENTS simulation_scripting)"
    "find_package(lux-engine-simulation-system REQUIRED COMPONENTS system)"
    "find_package(lux-engine-simulation-ecs REQUIRED COMPONENTS core)"
    "find_package(lux-engine-function REQUIRED COMPONENTS script_core)"
    "find_package(lux-engine-resource REQUIRED COMPONENTS asset)"
    "find_package(lux-engine-resource-script REQUIRED COMPONENTS script_asset)"
    "find_package(lux-engine-world REQUIRED COMPONENTS world)"
)
lux_classify_target(
    TARGET  simulation_script
    LAYER   SIMULATION
    PRODUCT RUNTIME
    ROLE    DOMAIN
)

add_executable(
    simulation_script_description_test
    test/script_system_description_test.cpp
)
target_link_libraries(
    simulation_script_description_test
    PRIVATE lux::engine::simulation::simulation_script
)
if(MSVC)
    target_compile_options(simulation_script_description_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_description_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_script_description_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME simulation_script_description_test
    COMMAND simulation_script_description_test
)

add_executable(
    simulation_script_runtime_test
    test/script_system_runtime_test.cpp
)
target_link_libraries(
    simulation_script_runtime_test
    PRIVATE lux::engine::simulation::simulation_script
)
if(MSVC)
    target_compile_options(simulation_script_runtime_test PRIVATE /UNDEBUG)
else()
    target_compile_options(simulation_script_runtime_test PRIVATE -UNDEBUG)
endif()
lux_classify_target(
    TARGET  simulation_script_runtime_test
    LAYER   SIMULATION
    PRODUCT TEST
    ROLE    DOMAIN
)
add_test(
    NAME simulation_script_runtime_test
    COMMAND simulation_script_runtime_test
)
